package io.github.abdoeid.phonecam.ui.main

import com.journeyapps.barcodescanner.CaptureActivity

/**
 * The QR scanner, in portrait.
 *
 * The scanner library declares its own capture activity as `sensorLandscape`, so
 * the camera view opens sideways while the rest of the app -- and the QR code on
 * the laptop -- is portrait. This subclass is declared in the app's manifest with
 * a portrait screen orientation, so scanning looks like the rest of PhoneCam.
 */
class QrScanActivity : CaptureActivity()
