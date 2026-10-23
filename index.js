/* //get enabled and post disbled error 
const express = require('express');
const { createProxyMiddleware } = require('http-proxy-middleware');
const cors = require('cors');
const path = require('path'); // 1. Add this built-in node module at the top

const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());

// 2. CHANGE THIS ROUTE to send your HTML file instead of the old string message
app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

// Forward requests directed to /proxy?url=https://example.com
app.use('/proxy', (req, res, next) => {
  const targetUrl = req.query.url;
  if (!targetUrl) {
    return res.status(400).json({ error: 'Missing "url" query parameter' });
  }

  createProxyMiddleware({
    target: targetUrl,
    changeOrigin: true,
    pathRewrite: (path, req) => '',
  })(req, res, next);
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Proxy running on port ${PORT}`);
});*/

const express = require('express');
const { createProxyMiddleware } = require('http-proxy-middleware');
const cors = require('cors');
const path = require('path');

const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());

// 1. Parse JSON and URL-encoded body structures sent by the SIM800L module
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// In-memory array storage to hold a running history of tracking events
let trackingLogsList = [];

// Serve your dashboard on the primary home route
app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, 'index.html'));
});

// 2. EXPOSE THE LOGS ENDPOINT exactly where your script looks for it: /logs.json
app.get('/logs.json', (req, res) => {
  res.json(trackingLogsList);
});

// 3. LISTEN TO POST REQUESTS coming from the tracking device
app.post('/', (req, res) => {
  console.log("Telemetry packet received from hardware:", req.body);

  // Extract variables or provide fallbacks if parameters are incomplete
  const newLog = {
    timestamp: new Date().toLocaleTimeString(), // Attaches live server time
    deviceId: req.body.deviceId || 'SIM800L-Relay',
    lat: req.body.lat,
    lon: req.body.lon,
    speed: req.body.speed,
    sats: req.body.sats,
    batt: req.body.batt,
    sos: req.body.isSos
  };

  // Add the log packet to the front of the list so new events display first
  trackingLogsList.unshift(newLog);

  // Limit memory buffer array footprint size so it won't crash your server over time
  if (trackingLogsList.length > 50) {
    trackingLogsList.pop();
  }

  // Send successful 200 OK verification code back to the hardware board
  res.status(200).send("Telemetry saved successfully.");
});

// Maintain your proxy endpoint capabilities if you need them for other links
app.use('/proxy', (req, res, next) => {
  const targetUrl = req.query.url;
  if (!targetUrl) {
    return res.status(400).json({ error: 'Missing "url" query parameter' });
  }

  createProxyMiddleware({
    target: targetUrl,
    changeOrigin: true,
    pathRewrite: (path, req) => '',
  })(req, res, next);
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Hardware relay engine active on port ${PORT}`);
});

