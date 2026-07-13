# inkterface-tweaks
This is a mostly vibe coded edit of the device firmware from the Steam Machine [Inkterface](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface) e-ink faceplate to support the ESP32 Feather V2 and enable partial refresh on the display. The following changes were made:
- Changed the battery monitoring code when building with the ESP32 Feather V2 build to use a simple voltage reading as it doesn't have the MAX17048 chip
- Switched to the GxEPD2 driver which supports partial refresh on the display and added a build flag for this feature (PARTIAL_REFRESH)
- Since a full refresh is still needed for display health a build flag was added to configure the cadence in minutes (FULL_REFRESH_INTERVAL_MIN, defaults to 15 mins)
- Added an experimental shorter invert pulse screen refresh as a build flag (INVERT_PULSE_CLEANUP).  This uses the above cadence and replaces the normal full refresh outside of boot and disconnect/reconnect events.
- Changed the platformio.ini file to reflect the ESP32 Feather V2 and use the updated pin mappings (Note: See the attached wiring image, if you wire things differently you'll have to update the same file)

To use just replace the main.ino and platformio.ini files from the main project with these before flashing.  The INVERT_PULSE_CLEANUP option is disabled by default since I'm not sure what the long term implications of using this are versus the normal method. Use the following build commands based on your ESP32 Feather version:
- ESP32 Feather V2 (Default)
  - pio run -t upload
- ESP32-S3 Feather (Untested as I don't have this version)
  - pio run -e panel_s3 -t upload

Disclaimer: This firmware changes the way your e-ink display functions and is not necessarily in line with recommended manufacturer guidelines.  You acknowledge that you are running this at your own risk and I'm not responsible for any issues that may arise from running this firmware or any variations.  

Partial refresh:

![Partial refresh gif](/images/partial-refresh.gif)

Invert pulse refresh:

![Invert pulse screen refresh](/images/invert-pulse.gif)
