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

var reminders = require('./reminders');
var notes = require('./notes');

Pebble.addEventListener('ready', function() {
  console.log('PWAI ready');
});

Pebble.addEventListener('appmessage', function(e) {
  var data = e.payload;

  // Reminder commands (set / list / delete)
  if (reminders.handleReminderMessage(data)) {
    return;
  }

  // Save voice note as Rebble timeline pin
  if (data.NOTE_TEXT) {
    notes.saveNote(data.NOTE_TEXT, function(err) {
      Pebble.sendAppMessage({ NOTE_SAVED: err ? 0 : 1 }, function() {
        console.log('NOTE_SAVED ack sent');
      }, function() {
        console.error('NOTE_SAVED ack failed');
      });
    });
  }
});
