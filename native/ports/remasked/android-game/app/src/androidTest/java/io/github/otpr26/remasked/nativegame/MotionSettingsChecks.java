package io.github.otpr26.remasked.nativegame;
import org.json.JSONObject;
final class MotionSettingsChecks {
    static void run() throws Exception {
        for(String mode:new String[]{"controller_gyroscope","controller_accelerometer","automatic","right_stick","disabled"}){
            JSONObject value=new JSONObject("{\"aim\":{\"source\":\""+mode+"\",\"invert_x\":true,\"invert_y\":false,\"gyroscope_sensitivity\":1.25}}");
            boolean legacy=mode.equals("controller_gyroscope")||mode.equals("controller_accelerometer");
            if(ReMaskedMotionSettings.normalize(value)!=legacy)throw new AssertionError("Wrong motion migration");
            JSONObject aim=value.getJSONObject("aim");
            if(!aim.getString("source").equals(legacy?"controller_motion":mode)||!aim.getBoolean("invert_x")||aim.getBoolean("invert_y")||aim.getDouble("gyroscope_sensitivity")!=1.25)throw new AssertionError("Motion migration changed user preferences");
            if(ReMaskedMotionSettings.normalize(value))throw new AssertionError("Motion migration is not idempotent");
        }
        if(ReMaskedMotionSettings.normalize(new JSONObject()))throw new AssertionError("Created absent settings");
    }
}
