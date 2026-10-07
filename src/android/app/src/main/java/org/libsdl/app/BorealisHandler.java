// SPDX-FileCopyrightText: 2023 xfangfang, XITRIX and other borealis contributors
// SPDX-License-Identifier: Apache-2.0
// From borealis (https://github.com/XITRIX/borealis, android-project, commit 8c1f1093); see src/harbor_setup/NOTICE.
package org.libsdl.app;

import android.app.Activity;
import android.os.Handler;
import android.os.Message;
import android.util.Log;
import android.view.Window;
import android.view.WindowManager;

public class BorealisHandler extends Handler {
    @Override
    public void handleMessage(Message msg) {
        super.handleMessage(msg);
        switch (msg.what) {
            case 0:
                Window window = ((Activity)msg.obj).getWindow();
                WindowManager.LayoutParams lp = window.getAttributes();
                lp.screenBrightness = msg.arg1 / 255.0f;
                if (lp.screenBrightness < 0.04) lp.screenBrightness = 0.04f;
                window.setAttributes(lp);
                break;
        }
    }
}