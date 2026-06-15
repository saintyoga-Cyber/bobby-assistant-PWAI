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

var timeline = require('../actions/timeline');

// Store reminders in localStorage
var REMINDERS_STORAGE_KEY = 'bobby_reminders';
// Keep expired reminders for 24 hours before cleanup
var EXPIRED_REMINDER_TTL = 24 * 60 * 60 * 1000; // 24 hours in milliseconds

function loadReminders() {
  var stored = localStorage.getItem(REMINDERS_STORAGE_KEY);
  return stored ? JSON.parse(stored) : [];
}

function saveReminders(reminders) {
  localStorage.setItem(REMINDERS_STORAGE_KEY, JSON.stringify(reminders));
}

function cleanupExpiredReminders() {
  var reminders = loadReminders();
  var now = new Date();
  var cutoffTime = now.getTime() - EXPIRED_REMINDER_TTL;
  
  // Filter out reminders that are older than the cutoff time
  var activeReminders = reminders.filter(function(reminder) {
    var reminderTime = new Date(reminder.time).getTime();
    return reminderTime > cutoffTime;
  });
  
  // If we removed any reminders, save the updated list
  if (activeReminders.length < reminders.length) {
    console.log("Cleaned up " + (reminders.length - activeReminders.length) + " expired reminders");
    saveReminders(activeReminders);
  }
  
  return activeReminders;
}

function addReminder(text, time) {
  // Clean up expired reminders first
  cleanupExpiredReminders();
  
  var date = (new Date(time)).toISOString();
  console.log("Setting a reminder: \"" + text + "\" at " + date);
  var reminderId = "bobby-reminder-" + Math.random();
  
  var pin = {
    "id": reminderId,
    "time": date,
    "layout": {
      "type": "genericPin",
      "title": text,
      "tinyIcon": "system://images/NOTIFICATION_REMINDER"
    },
    "reminders": [{
      "time": date,
      "layout": {
        "type": "genericReminder",
        "title": text,
        "tinyIcon": "system://images/NOTIFICATION_REMINDER"
      }
    }]
  };

  // Insert into timeline first - if this fails it will throw
  timeline.insertUserPin(pin);

  // Store reminder locally
  var reminders = loadReminders();
  reminders.push({
    id: reminderId,
    time: date,
    what: text
  });
  saveReminders(reminders);
  
  return reminderId;
}

function deleteReminder(id) {
  // Clean up expired reminders first
  cleanupExpiredReminders();
  
  var reminders = loadReminders();
  var reminderIndex = -1;
  for (var i = 0; i < reminders.length; i++) {
    if (reminders[i].id === id) {
      reminderIndex = i;
      break;
    }
  }
  
  if (reminderIndex === -1) {
    return false;
  }
  
  // Remove from timeline first - if this fails it will throw
  timeline.deleteUserPin(id);

  // Remove from local storage
  reminders.splice(reminderIndex, 1);
  saveReminders(reminders);
  return true;
}

function getAllReminders() {
  // Clean up expired reminders before returning the list
  var reminders = cleanupExpiredReminders();
  
  // Sort reminders by time
  reminders.sort(function(a, b) {
    return new Date(a.time) - new Date(b.time);
  });
  
  // Only return future reminders
  var now = new Date();
  return reminders.filter(function(r) {
    return new Date(r.time) > now;
  });
}

var PENDING_PINS_KEY = 'bobby_pending_pins';

function loadPendingPins() {
  var stored = localStorage.getItem(PENDING_PINS_KEY);
  return stored ? JSON.parse(stored) : [];
}

function savePendingPins(pins) {
  localStorage.setItem(PENDING_PINS_KEY, JSON.stringify(pins));
}

function addReminderFromWatch(id, timeUnix, text) {
  var date = new Date(timeUnix * 1000).toISOString();
  var pin = {
    "id": id,
    "time": date,
    "layout": {
      "type": "genericPin",
      "title": text,
      "tinyIcon": "system://images/NOTIFICATION_REMINDER"
    },
    "reminders": [{
      "time": date,
      "layout": {
        "type": "genericReminder",
        "title": text,
        "tinyIcon": "system://images/NOTIFICATION_REMINDER"
      }
    }]
  };
  timeline.insertUserPin(pin, function(err) {
    if (err) {
      console.log('addReminderFromWatch: pin insert failed, queuing: ' + err);
      var pending = loadPendingPins();
      // Avoid duplicates
      for (var i = 0; i < pending.length; i++) {
        if (pending[i].id === id) return;
      }
      pending.push({id: id, time: timeUnix, text: text});
      savePendingPins(pending);
    } else {
      console.log('addReminderFromWatch: pin created: ' + id);
      Pebble.sendAppMessage({'REMINDER_PIN_SYNCED': id});
    }
  });
  // Also store locally for getAllReminders()
  var reminders = loadReminders();
  var exists = false;
  for (var i = 0; i < reminders.length; i++) {
    if (reminders[i].id === id) { exists = true; break; }
  }
  if (!exists) {
    reminders.push({id: id, time: date, what: text});
    saveReminders(reminders);
  }
}

function flushPendingPins() {
  var pending = loadPendingPins();
  if (pending.length === 0) return;
  console.log('flushPendingPins: ' + pending.length + ' pending');
  var remaining = pending.slice();
  function tryNext(idx) {
    if (idx >= remaining.length) {
      savePendingPins(remaining);
      return;
    }
    var p = remaining[idx];
    var date = new Date(p.time * 1000).toISOString();
    var pin = {
      "id": p.id,
      "time": date,
      "layout": {
        "type": "genericPin",
        "title": p.text,
        "tinyIcon": "system://images/NOTIFICATION_REMINDER"
      },
      "reminders": [{
        "time": date,
        "layout": {
          "type": "genericReminder",
          "title": p.text,
          "tinyIcon": "system://images/NOTIFICATION_REMINDER"
        }
      }]
    };
    timeline.insertUserPin(pin, function(err) {
      if (!err) {
        console.log('flushPendingPins: synced ' + p.id);
        remaining.splice(idx, 1);
        Pebble.sendAppMessage({'REMINDER_PIN_SYNCED': p.id});
        tryNext(idx);  // next item shifted into this position
      } else {
        console.log('flushPendingPins: still failing for ' + p.id);
        tryNext(idx + 1);
      }
    });
  }
  tryNext(0);
}

module.exports = {
  addReminder: addReminder,
  deleteReminder: deleteReminder,
  getAllReminders: getAllReminders,
  addReminderFromWatch: addReminderFromWatch,
  flushPendingPins: flushPendingPins
};