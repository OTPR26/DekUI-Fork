// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.otpr26.remasked.nativegame;
import android.content.Context;
import android.graphics.*;
import android.view.View;
/** Original ReMasked mask artwork and placement, shared with the 0.4.4 launcher. */
final class ReMaskedLauncherBackdrop extends View {
 private final Bitmap artwork;
 private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
 ReMaskedLauncherBackdrop(Context context){super(context);artwork=BitmapFactory.decodeResource(getResources(),org.triaevum.android.R.drawable.dekui_launcher_masks);setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);}
 @Override protected void onDraw(Canvas canvas){canvas.drawColor(Color.rgb(8,7,13));if(artwork==null)return;float scale=Math.min(getWidth()*.56f/artwork.getWidth(),getHeight()*.78f/artwork.getHeight());float w=artwork.getWidth()*scale,h=artwork.getHeight()*scale,cx=getWidth()*.30f,cy=getHeight()*.55f;canvas.drawBitmap(artwork,null,new RectF(cx-w/2,cy-h/2,cx+w/2,cy+h/2),paint);}
}
