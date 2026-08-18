package com.motoctrl.app.ble

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/** Registers BleWatchModule. Manually added in MainApplication.kt's package
 * list -- this isn't a separate npm package, so there's nothing to
 * autolink. */
class BleWatchPackage : ReactPackage {
    override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> =
        listOf(BleWatchModule(reactContext))

    @Suppress("DEPRECATION") // no view managers here; the newer on-demand API is view-manager-only surface we don't need
    override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> =
        emptyList()
}
