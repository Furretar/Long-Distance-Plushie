const express = require('express');
const mqtt = require('mqtt');

const app = express();
app.use(express.json());

const client = mqtt.connect('mqtts://h862f16c.ala.us-east-1.emqxsl.com:8883', {
  username: 'a',
  password: 'a',
  rejectUnauthorized: false
});

client.on('connect', () => console.log('Connected to EMQX'));
client.on('error', (e) => console.error('MQTT error:', e.message));

app.post('/publish', (req, res) => {
  const { topic, message } = req.body;
  if (!topic || !message) return res.status(400).json({ error: 'topic and message required' });
  client.publish(topic, message, (err) => {
    if (err) return res.status(500).json({ error: err.message });
    res.json({ success: true, topic, message });
  });
});

app.get('/health', (req, res) => res.json({ mqtt: client.connected }));

app.listen(3000, () => console.log('Bridge running on port 3000'));