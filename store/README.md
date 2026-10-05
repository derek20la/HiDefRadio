# Store graphics

Pictures for the Google Play listing (not part of the app).

- `icon_512.png` - the app icon, 512 x 512. It is the centre 72 x 72 of the adaptive launcher
  icon (`app/src/main/res/drawable/ic_launcher_*.xml`), drawn as a full square; Google Play
  rounds the corners itself.
- `feature_graphic_1024x500.png` - the banner at the top of the store page.
- `feature_graphic.html` - the source of the banner. Open it in a browser window 1024 x 500
  pixels in size and take a screenshot to remake the PNG. It uses the app's own display font
  (DSEG7 Classic Bold, `app/src/main/res/font/`) and the Inter font for the text.
