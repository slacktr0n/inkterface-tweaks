# inkterface-tweaks
This is a mostly vibe coded edit of the device firmware from the Steam Machine [Inkterface](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface) e-ink faceplate to support the ESP32 Feather V2 and enable partial refresh on the display. The following changes were made:
- Removed the battery fuel gauge code since the ESP32 Feather V2 doesn't have the MAX17048 chip and replaced with a simple voltage reading
- Switched to the GxEPD2 driver which supports partial refresh on the display and added a build flag for this feature (PARTIAL_REFRESH)
- Since a full refresh is still needed for display health a build flag was added to configure the cadence in minutes (FULL_REFRESH_INTERVAL_MIN, defaults to 15 mins)
- Added an experimental shorter invert pulse screen refresh as a build flag (INVERT_PULSE_CLEANUP).  This uses the above cadence and replaces the normal full refresh outside of boot and disconnect/reconnect events.
- Changed the platformio.ini file to reflect the ESP32 Feather V2 and use the updated pin mappings (Note: See the attached wiring image, if you wire things differently you'll have to update the same file)

To use just replace the main.ino and platformio.ini files from the main project with these before flashing.

Disclaimer: This firmware changes the way your e-ink display functions and is not necessarily in line with recommended manufacturer guidelines.  You acknowledge that you are running this at your own risk and I'm not responsible for any issues that may arise from running this firmware or any variations.  
