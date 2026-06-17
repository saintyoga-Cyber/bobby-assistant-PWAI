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

// City / region name → IANA timezone ID
var TZ_MAP = {
  // US
  'hawaii': 'Pacific/Honolulu',
  'honolulu': 'Pacific/Honolulu',
  'alaska': 'America/Anchorage',
  'anchorage': 'America/Anchorage',
  'los angeles': 'America/Los_Angeles',
  'la': 'America/Los_Angeles',
  'san francisco': 'America/Los_Angeles',
  'seattle': 'America/Los_Angeles',
  'portland': 'America/Los_Angeles',
  'las vegas': 'America/Los_Angeles',
  'phoenix': 'America/Phoenix',
  'denver': 'America/Denver',
  'salt lake city': 'America/Denver',
  'albuquerque': 'America/Denver',
  'dallas': 'America/Chicago',
  'houston': 'America/Chicago',
  'chicago': 'America/Chicago',
  'minneapolis': 'America/Chicago',
  'new orleans': 'America/Chicago',
  'new york': 'America/New_York',
  'nyc': 'America/New_York',
  'boston': 'America/New_York',
  'miami': 'America/New_York',
  'atlanta': 'America/New_York',
  'washington': 'America/New_York',
  'dc': 'America/New_York',
  'philadelphia': 'America/New_York',
  // Canada
  'toronto': 'America/Toronto',
  'montreal': 'America/Toronto',
  'vancouver': 'America/Vancouver',
  'calgary': 'America/Edmonton',
  'edmonton': 'America/Edmonton',
  // Latin America
  'mexico city': 'America/Mexico_City',
  'cancun': 'America/Cancun',
  'bogota': 'America/Bogota',
  'lima': 'America/Lima',
  'santiago': 'America/Santiago',
  'buenos aires': 'America/Argentina/Buenos_Aires',
  'sao paulo': 'America/Sao_Paulo',
  'rio': 'America/Sao_Paulo',
  'rio de janeiro': 'America/Sao_Paulo',
  // Europe
  'london': 'Europe/London',
  'dublin': 'Europe/Dublin',
  'lisbon': 'Europe/Lisbon',
  'madrid': 'Europe/Madrid',
  'barcelona': 'Europe/Madrid',
  'paris': 'Europe/Paris',
  'brussels': 'Europe/Brussels',
  'amsterdam': 'Europe/Amsterdam',
  'berlin': 'Europe/Berlin',
  'frankfurt': 'Europe/Berlin',
  'munich': 'Europe/Berlin',
  'vienna': 'Europe/Vienna',
  'zurich': 'Europe/Zurich',
  'geneva': 'Europe/Zurich',
  'rome': 'Europe/Rome',
  'milan': 'Europe/Rome',
  'stockholm': 'Europe/Stockholm',
  'oslo': 'Europe/Oslo',
  'copenhagen': 'Europe/Copenhagen',
  'helsinki': 'Europe/Helsinki',
  'warsaw': 'Europe/Warsaw',
  'prague': 'Europe/Prague',
  'budapest': 'Europe/Budapest',
  'bucharest': 'Europe/Bucharest',
  'athens': 'Europe/Athens',
  'istanbul': 'Europe/Istanbul',
  'moscow': 'Europe/Moscow',
  // Middle East / Africa
  'dubai': 'Asia/Dubai',
  'abu dhabi': 'Asia/Dubai',
  'riyadh': 'Asia/Riyadh',
  'tehran': 'Asia/Tehran',
  'tel aviv': 'Asia/Jerusalem',
  'jerusalem': 'Asia/Jerusalem',
  'cairo': 'Africa/Cairo',
  'nairobi': 'Africa/Nairobi',
  'johannesburg': 'Africa/Johannesburg',
  'lagos': 'Africa/Lagos',
  'casablanca': 'Africa/Casablanca',
  // Asia / Pacific
  'karachi': 'Asia/Karachi',
  'mumbai': 'Asia/Kolkata',
  'delhi': 'Asia/Kolkata',
  'india': 'Asia/Kolkata',
  'kolkata': 'Asia/Kolkata',
  'dhaka': 'Asia/Dhaka',
  'colombo': 'Asia/Colombo',
  'kathmandu': 'Asia/Kathmandu',
  'tashkent': 'Asia/Tashkent',
  'almaty': 'Asia/Almaty',
  'bangkok': 'Asia/Bangkok',
  'jakarta': 'Asia/Jakarta',
  'singapore': 'Asia/Singapore',
  'kuala lumpur': 'Asia/Kuala_Lumpur',
  'kl': 'Asia/Kuala_Lumpur',
  'manila': 'Asia/Manila',
  'hong kong': 'Asia/Hong_Kong',
  'taipei': 'Asia/Taipei',
  'beijing': 'Asia/Shanghai',
  'shanghai': 'Asia/Shanghai',
  'china': 'Asia/Shanghai',
  'tokyo': 'Asia/Tokyo',
  'osaka': 'Asia/Tokyo',
  'japan': 'Asia/Tokyo',
  'seoul': 'Asia/Seoul',
  'korea': 'Asia/Seoul',
  'sydney': 'Australia/Sydney',
  'melbourne': 'Australia/Melbourne',
  'brisbane': 'Australia/Brisbane',
  'perth': 'Australia/Perth',
  'adelaide': 'Australia/Adelaide',
  'auckland': 'Pacific/Auckland',
  'new zealand': 'Pacific/Auckland',
  'fiji': 'Pacific/Fiji',
  'tahiti': 'Pacific/Tahiti',
};

function lookupTz(query) {
  var q = query.toLowerCase().replace(/[^a-z ]/g, '').trim();
  if (TZ_MAP[q]) return TZ_MAP[q];
  // Try partial match
  for (var key in TZ_MAP) {
    if (q.indexOf(key) >= 0 || key.indexOf(q) >= 0) return TZ_MAP[key];
  }
  return null;
}

function format12h(h, m) {
  var suffix = h >= 12 ? 'PM' : 'AM';
  var h12 = h % 12;
  if (h12 === 0) h12 = 12;
  var ms = m < 10 ? '0' + m : '' + m;
  return h12 + ':' + ms + ' ' + suffix;
}

exports.handleTzQuery = function(data) {
  if (!('TZ_QUERY' in data)) return false;
  var query = data.TZ_QUERY || '';
  var tzId = lookupTz(query);
  if (!tzId) {
    Pebble.sendAppMessage({ TZ_RESPONSE: 'City not recognised.' });
    return true;
  }

  // Use WorldTimeAPI for accurate DST-aware local time
  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    var text;
    if (this.status >= 200 && this.status < 300) {
      try {
        var d = JSON.parse(this.responseText);
        // datetime: "2026-06-16T14:30:00-10:00"
        var dt = d.datetime;
        var h = parseInt(dt.substring(11, 13), 10);
        var m = parseInt(dt.substring(14, 16), 10);
        var abbr = d.abbreviation || '';
        text = format12h(h, m) + (abbr ? ' ' + abbr : '');
      } catch (e) {
        text = 'Time unavailable.';
      }
    } else {
      text = 'Time unavailable.';
    }
    Pebble.sendAppMessage(
      { TZ_RESPONSE: text },
      function() { console.log('TZ_RESPONSE sent'); },
      function() { console.error('TZ_RESPONSE failed'); }
    );
  };
  xhr.onerror = function() {
    Pebble.sendAppMessage({ TZ_RESPONSE: 'Time unavailable.' });
  };
  xhr.open('GET', 'https://worldtimeapi.org/api/timezone/' + tzId);
  xhr.send();
  return true;
};
