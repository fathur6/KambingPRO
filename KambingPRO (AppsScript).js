// Google Apps Script to receive data from ESP32 and log to Google Sheet

// IMPORTANT: Replace with your actual Google Spreadsheet ID
const SPREADSHEET_ID = "1u6qhpIC5tHcCrUh8WNYn-tRyFY-UWu1i-ibd34V7cA0"; // <<< YOUR SPREADSHEET ID HERE

/**
 * Handles HTTP POST requests. This function is triggered when the ESP32 sends data.
 * @param {Object} e The event parameter for a POST request.
 * @return {ContentService.TextOutput} A text output indicating success or failure.
 */
function doPost(e) {
  try {
    // Parse the JSON payload from the ESP32
    const payload = JSON.parse(e.postData.contents);

    // The "thing" field in the JSON (e.g., "RAB001") must exactly match a sheet name (tab name) in your spreadsheet.
    const deviceName = payload.thing;
    if (!deviceName) {
      Logger.log("Error: 'thing' field missing in payload.");
      return ContentService.createTextOutput("Error: 'thing' field missing in payload.")
                           .setMimeType(ContentService.MimeType.TEXT);
    }

    const ss = SpreadsheetApp.openById(SPREADSHEET_ID);
    const sheet = ss.getSheetByName(deviceName);

    if (!sheet) {
      Logger.log("Error: No sheet named '" + deviceName + "' found.");
      return ContentService.createTextOutput("Error: No sheet named '" + deviceName + "' found.")
                           .setMimeType(ContentService.MimeType.TEXT);
    }

    // Ensure the sheet has a header row or is prepared for data.
    // The order of elements in this 'row' array MUST match the column order in your sheet.
    // Assumed Column Order:
    // A: Timestamp
    // B: Ammonia (ppm)
    // C: Temperature (°C)
    // D: Humidity (%)
    // E: Storage Tank Volume (L)
    // F: Pump Status (0 or 1)
    // G: Siren Status (0 or 1)
    // H: CCTV Status (0 or 1)
    // I: AUX Socket Status (0 or 1)
    
    const rowData = [
      payload.timestamp ? new Date(payload.timestamp) : null, // Convert ISO string to Date object; use null if timestamp is missing
      payload.ammonia,         // Will be 'undefined' if not in payload, resulting in a blank cell
      payload.temperature,
      payload.humidity,
      payload.storageTank,
      payload.storagePump,
      payload.siren,
      payload.cctv,
      payload.auxiliarySocket  // ESP32 sends 'auxiliarySocket'
    ];

    sheet.appendRow(rowData);
    Logger.log("Data appended to sheet: " + deviceName + ", Row: " + rowData.join(", "));

    return ContentService.createTextOutput("OK: Data received for " + deviceName)
                         .setMimeType(ContentService.MimeType.TEXT);

  } catch (err) {
    Logger.log("Error processing POST request: " + err.toString() + "\nStack: " + err.stack);
    // It's good to log the actual error content as well if possible, from e.postData.contents, in case of JSON parsing errors
    if (e && e.postData && e.postData.contents) {
        Logger.log("Received payload (potential error source): " + e.postData.contents);
    }
    return ContentService.createTextOutput("Error: " + err.toString())
                         .setMimeType(ContentService.MimeType.TEXT);
  }
}

// Optional: A simple function to test deployment and permissions from the Apps Script editor
function testScript() {
  const ss = SpreadsheetApp.openById(SPREADSHEET_ID);
  Logger.log("Spreadsheet Name: " + ss.getName());
  const sheet = ss.getSheetByName("RAB001"); // Or any valid sheet name you expect
  if (sheet) {
    Logger.log("Test sheet '" + sheet.getName() + "' found.");
  } else {
    Logger.log("Test sheet 'RAB001' not found.");
  }
}