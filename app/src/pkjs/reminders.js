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

var reminders = require('./lib/reminders');

function handleReminderMessage(data) {
  if (data.OFFLINE_REMINDER_TEXT && data.OFFLINE_REMINDER_TIME) {
    console.log('Offline reminder: ' + data.OFFLINE_REMINDER_TEXT);
    reminders.addReminder(data.OFFLINE_REMINDER_TEXT, data.OFFLINE_REMINDER_TIME * 1000);
    return true;
  }
  if (data.REMINDER_LIST_REQUEST) {
    var allReminders = reminders.getAllReminders();
    
    // Convert from storage format to menu format
    var menuReminders = allReminders.map(function(r) {
      return {
        id: r.id,
        text: r.what,
        time: new Date(r.time)
      };
    });

    // First send the count
    Pebble.sendAppMessage({'REMINDER_COUNT': menuReminders.length}, function() {
      // On success start sending individual reminders
      sendNextReminder(0);
    }, function() {
      console.error('Failed to send REMINDER_COUNT to watch');
    });

    // Then send each reminder
    function sendNextReminder(index) {
      if (index >= menuReminders.length) return;

      var reminder = menuReminders[index];
      Pebble.sendAppMessage({
        'REMINDER_TEXT': reminder.text,
        'REMINDER_ID': reminder.id,
        'REMINDER_TIME': Math.floor(reminder.time.getTime() / 1000)
      }, function() {
        sendNextReminder(index + 1);
      }, function() {
        // Skip on failure rather than retrying forever
        console.error('Failed to send reminder ' + index + ', skipping');
        sendNextReminder(index + 1);
      });
    }
    return true;
  } else if (data.REMINDER_DELETE) {
    var id = data.REMINDER_DELETE;
    try {
      reminders.deleteReminder(id);
    } catch (err) {
      console.error('Failed to delete reminder:', err);
    }
    return true;
  }
  return false;
}

module.exports = {
  handleReminderMessage: handleReminderMessage
}; 
