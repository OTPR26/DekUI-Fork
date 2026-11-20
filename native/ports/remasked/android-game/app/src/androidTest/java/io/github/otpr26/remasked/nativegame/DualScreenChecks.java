package io.github.otpr26.remasked.nativegame;
final class DualScreenChecks {
 static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
 static void run(){
  for(float[] size:new float[][]{{1240,1080},{1920,1080},{1080,1240},{640,480}}){
   float w=size[0],h=size[1],pw=Math.min(w,h*4f/3f),ph=pw*3f/4f,l=(w-pw)/2,t=(h-ph)/2;
   int[] c=ReMaskedDualScreen.mapTouch(w,h,w/2,h/2);check(c!=null&&c[0]==160&&c[1]==120,"Lower picture center must map to native coordinates");
   c=ReMaskedDualScreen.mapTouch(w,h,l,t);check(c!=null&&c[0]==0&&c[1]==0,"Picture origin");
   check(ReMaskedDualScreen.mapTouch(w,h,l+pw,t)==null,"Reject right border");
   check(ReMaskedDualScreen.mapTouch(w,h,l,t+ph)==null,"Reject bottom border");
   if(t>1)check(ReMaskedDualScreen.mapTouch(w,h,w/2,t-1)==null,"Reject top letterbox");
   if(l>1)check(ReMaskedDualScreen.mapTouch(w,h,l-1,h/2)==null,"Reject side letterbox");
  }
  check(ReMaskedDualScreen.mapTouch(0,0,0,0)==null,"Reject destroyed surface");
  check(ReMaskedDualScreen.mapTouch(1240,1080,Float.NaN,1)==null,"Reject invalid touch");
 }
}
