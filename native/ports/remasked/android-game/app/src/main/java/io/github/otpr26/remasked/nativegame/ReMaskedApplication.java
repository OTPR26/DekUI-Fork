// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.app.*;
import android.os.*;
import java.util.HashSet;
public final class ReMaskedApplication extends Application {
    @Override public void onCreate(){super.onCreate();
        HashSet<Activity> visible=new HashSet<>();Handler handler=new Handler(Looper.getMainLooper());
        Runnable settle=()->{if(!visible.isEmpty())return;
            ActivityManager manager=(ActivityManager)getSystemService(ACTIVITY_SERVICE);
            java.util.List<ActivityManager.RunningAppProcessInfo> processes=manager.getRunningAppProcesses();
            if(processes!=null)for(ActivityManager.RunningAppProcessInfo p:processes)
                if(p.uid==android.os.Process.myUid()&&p.importance<=ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND)return;
            ReMaskedAppIcons.reconcile(this);
        };
        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks(){
            public void onActivityStarted(Activity a){visible.add(a);handler.removeCallbacks(settle);}
            public void onActivityStopped(Activity a){visible.remove(a);if(visible.isEmpty())handler.postDelayed(settle,500);}
            public void onActivityResumed(Activity a){ReMaskedAppIcons.applyTaskIcon(a);}
            public void onActivityCreated(Activity a,Bundle b){}public void onActivityPaused(Activity a){}
            public void onActivitySaveInstanceState(Activity a,Bundle b){}public void onActivityDestroyed(Activity a){visible.remove(a);}
        });
    }
}
