/**
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// WMO Weather interpretation codes → human-readable condition
var WMO_CODES = {
  0: 'Clear Sky',
  1: 'Mainly Clear', 2: 'Partly Cloudy', 3: 'Overcast',
  45: 'Fog', 48: 'Icy Fog',
  51: 'Light Drizzle', 53: 'Drizzle', 55: 'Heavy Drizzle',
  61: 'Light Rain', 63: 'Rain', 65: 'Heavy Rain',
  71: 'Light Snow', 73: 'Snow', 75: 'Heavy Snow',
  77: 'Snow Grains',
  80: 'Light Showers', 81: 'Showers', 82: 'Heavy Showers',
  85: 'Snow Showers', 86: 'Heavy Snow Showers',
  95: 'Thunderstorm',
  96: 'Thunderstorm w/ Hail', 99: 'Thunderstorm w/ Heavy Hail'
};

function wmoDesc(code) {
  return WMO_CODES[code] || 'Unknown';
}

function round1(v) {
  return Math.round(v * 10) / 10;
}

function fetchWeather(dayOffset, callback) {
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      var lat = pos.coords.latitude.toFixed(4);
      var lon = pos.coords.longitude.toFixed(4);
      var url = 'https://api.open-meteo.com/v1/forecast' +
        '?latitude=' + lat + '&longitude=' + lon +
        '&current=temperature_2m,weather_code' +
        '&daily=temperature_2m_max,temperature_2m_min,weather_code' +
        '&temperature_unit=celsius&forecast_days=2&timezone=auto';

      var xhr = new XMLHttpRequest();
      xhr.onload = function() {
        if (this.status < 200 || this.status >= 300) {
          callback('Weather unavailable.');
          return;
        }
        try {
          var data = JSON.parse(this.responseText);
          var text;
          if (dayOffset === 0) {
            var temp = round1(data.current.temperature_2m);
            var cond = wmoDesc(data.current.weather_code);
            text = temp + '°C, ' + cond + '.';
          } else {
            var hi = Math.round(data.daily.temperature_2m_max[1]);
            var lo = Math.round(data.daily.temperature_2m_min[1]);
            var cond2 = wmoDesc(data.daily.weather_code[1]);
            text = 'Tomorrow: ' + lo + '–' + hi + '°C, ' + cond2 + '.';
          }
          callback(text);
        } catch (e) {
          callback('Weather unavailable.');
        }
      };
      xhr.onerror = function() { callback('Weather unavailable.'); };
      xhr.open('GET', url);
      xhr.send();
    },
    function() { callback('Weather unavailable.'); },
    { timeout: 8000 }
  );
}

exports.handleWeatherRequest = function(data) {
  if (!('WEATHER_REQUEST' in data)) return false;
  var dayOffset = data.WEATHER_REQUEST ? 1 : 0;
  fetchWeather(dayOffset, function(text) {
    Pebble.sendAppMessage(
      { WEATHER_RESPONSE: text },
      function() { console.log('WEATHER_RESPONSE sent'); },
      function() { console.error('WEATHER_RESPONSE failed'); }
    );
  });
  return true;
};
