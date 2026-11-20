// SPDX-License-Identifier: GPL-2.0-or-later
// UIKit geometry and lower-screen input follow the ReTuned Duo template.
#import <TargetConditionals.h>
#import <UIKit/UIKit.h>
#include "apple_duo.h"
#include "fast/extension/remasked_portrait_layout.h"
#include <cstdlib>
#include "oot3d_menu_model.h"
#include "fast/extension/remasked_duo_layout.h"
#if TARGET_OS_IOS && !TARGET_OS_TV
#import "duo_layout_observer.h"
#include <SDL.h>
#include "ios/motion_orientation.h"
#include <imgui.h>
#include <SDL_syswm.h>
static ReMaskedDuo::Rect DuoRect(CGRect r){return {float(r.origin.x),float(r.origin.y),float(r.size.width),float(r.size.height)};}
@interface ReMaskedDuoTouchView : UIView
@end
@implementation ReMaskedDuoTouchView
- (BOOL)pointInside:(CGPoint)p withEvent:(UIEvent*)event {
    return !ReMaskedDual::MenuVisible.load()&&ReMaskedDuo::GameDisplay.MapTouch(p.x,p.y,ReMaskedDual::Active.load()).has_value();
}
- (void)send:(NSSet<UITouch*>*)touches down:(BOOL)down {
    CGPoint p=[touches.anyObject locationInView:self];
    auto touch=ReMaskedDuo::GameDisplay.MapTouch(p.x,p.y,down&&ReMaskedDual::Active.load());
    if(touch)ReMaskedDual::SetTouch(touch->X,touch->Y,true);else ReMaskedDual::SetTouch(0,0,false);
#if TARGET_OS_SIMULATOR
    if(std::getenv("REMASKED_DUO_TRACE_TOUCH"))NSLog(@"REMASKED_DUO_TOUCH down=%d view=%.1f,%.1f native=%d,%d mapped=%d",down,p.x,p.y,touch?int(touch->X):-1,touch?int(touch->Y):-1,touch.has_value());
#endif
}
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {[self send:t down:YES];}
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {[self send:t down:YES];}
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {[self send:t down:NO];}
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {[self send:t down:NO];}
@end
static ReMaskedDuoLayoutObserver* Observer;
static ReMaskedDuoTouchView* TouchView;
static __weak UIView* ObservedView;
bool ReMaskedAppleDuoAvailable(){return ReMaskedDuo::GameDisplay.Available();}
bool ReMaskedAppleDuoRequested(){return [NSUserDefaults.standardUserDefaults boolForKey:@"ReMasked.Native.DualScreen"];}
void ReMaskedAppleSetDuoRequested(bool enabled){
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"ReMasked.Native.DualScreen"];
    ReMaskedDual::Requested=enabled;
}
void ReMaskedAppleDuoFrame(){
    NSCAssert(NSThread.isMainThread,@"Duo layout must be observed on the UI thread");
    // UIKit may reactivate SDL's text field after a fold/focus change. Reconcile
    // the actual SDL state, rather than a cached desktop keyboard preference.
    const bool wantsText=ImGui::GetCurrentContext()&&ImGui::GetIO().WantTextInput;
    if(wantsText!=(SDL_IsTextInputActive()==SDL_TRUE)){
        if(wantsText)SDL_StartTextInput();else SDL_StopTextInput();
    }
    SDL_Window* window=SDL_GetKeyboardFocus();
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    UIView* view=nil;
    if(window&&SDL_GetWindowWMInfo(window,&info)&&info.subsystem==SDL_SYSWM_UIKIT)
        view=info.info.uikit.window.rootViewController.view;
    // UIKit may have no SDL keyboard focus on a touch-only device.
    if(!view)for(UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        if([scene isKindOfClass:UIWindowScene.class]&&scene.activationState==UISceneActivationStateForegroundActive)
            for(UIWindow* candidate in ((UIWindowScene*)scene).windows)
                if(candidate.isKeyWindow)view=candidate.rootViewController.view;
    if(view.window.windowScene){
        switch(view.window.windowScene.interfaceOrientation){
        case UIInterfaceOrientationPortrait:ReMaskedIOS::SceneMotionOrientation=SDL_ORIENTATION_PORTRAIT;break;
        case UIInterfaceOrientationPortraitUpsideDown:ReMaskedIOS::SceneMotionOrientation=SDL_ORIENTATION_PORTRAIT_FLIPPED;break;
        case UIInterfaceOrientationLandscapeLeft:ReMaskedIOS::SceneMotionOrientation=SDL_ORIENTATION_LANDSCAPE_FLIPPED;break;
        case UIInterfaceOrientationLandscapeRight:ReMaskedIOS::SceneMotionOrientation=SDL_ORIENTATION_LANDSCAPE;break;
        default:break;
        }
        // SDL2 predates scene-coordinate resizing. Keep its single UIKit window
        // aligned with the active scene when Duo folds or rotates.
        UIWindow* host=view.window;
        const CGRect frame=host.windowScene.coordinateSpace.bounds;
        if(!CGRectEqualToRect(host.frame,frame)){
            host.frame=frame;
            [host setNeedsLayout];[host layoutIfNeeded];
        }
        if(!CGRectEqualToRect(view.frame,host.bounds)){
            view.frame=host.bounds;
            [view setNeedsLayout];[view layoutIfNeeded];
        }
    }
    if(view&&view!=ObservedView){
        [Observer invalidate];[TouchView removeFromSuperview];ObservedView=view;
        TouchView=[[ReMaskedDuoTouchView alloc] initWithFrame:view.bounds];
        TouchView.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        TouchView.backgroundColor=UIColor.clearColor;TouchView.multipleTouchEnabled=NO;
        [view addSubview:TouchView];
        Observer=[[ReMaskedDuoLayoutObserver alloc] initWithView:view handler:^(ReMaskedDuoLayoutSnapshot* s){
            ReMaskedDuo::Geometry g;g.Available=s.phoneWithHinge;g.Closed=s.hingeStatus==0||s.hingeStatus==1;
            g.Bounds=DuoRect(s.bounds);g.Safe=DuoRect(s.safeBounds);
            ReMaskedIOS::PhonePortrait.Publish(g.Bounds,g.Safe,
                UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPhone&&!s.phoneWithHinge);
            for(NSValue* v in s.divisions){g.Division=DuoRect(v.CGRectValue);if(ReMaskedDuo::Divide(g).Dual())break;}
            for(NSValue* v in s.occlusions)g.Occlusions.push_back(DuoRect(v.CGRectValue));
            static CGRect lastBounds=CGRectNull,lastDivision=CGRectNull;
            static NSInteger lastStatus=-1;static BOOL lastAvailable=NO;
            const CGRect division=CGRectMake(g.Division.X,g.Division.Y,g.Division.Width,g.Division.Height);
            if(!CGRectEqualToRect(lastBounds,s.bounds)||!CGRectEqualToRect(lastDivision,division)||lastStatus!=s.hingeStatus||lastAvailable!=s.phoneWithHinge){
                NSLog(@"REMASKED_DUO available=%d status=%ld bounds=%@ division=%@ dual=%d",s.phoneWithHinge,(long)s.hingeStatus,NSStringFromCGRect(s.bounds),NSStringFromCGRect(division),ReMaskedDuo::Divide(g).Dual());
                NSLog(@"REMASKED_DUO_SCENE orientation=%ld scene=%@ window=%@ viewFrame=%@ transform=%@",(long)ObservedView.window.windowScene.interfaceOrientation,NSStringFromCGRect(ObservedView.window.windowScene.coordinateSpace.bounds),NSStringFromCGRect(ObservedView.window.bounds),NSStringFromCGRect(ObservedView.frame),NSStringFromCGAffineTransform(ObservedView.transform));
                const auto portrait=ReMaskedIOS::PhonePortrait.Pixels(s.bounds.size.width,s.bounds.size.height);
                NSLog(@"REMASKED_PORTRAIT idiom=%ld hinge=%d bounds=%@ safe=%@ active=%d game=%.1f,%.1f,%.1f,%.1f controls=%.1f,%.1f,%.1f,%.1f",(long)UIDevice.currentDevice.userInterfaceIdiom,s.phoneWithHinge,NSStringFromCGRect(s.bounds),NSStringFromCGRect(s.safeBounds),portrait.Active(),portrait.Game.X,portrait.Game.Y,portrait.Game.Width,portrait.Game.Height,portrait.Controls.X,portrait.Controls.Y,portrait.Controls.Width,portrait.Controls.Height);
                lastBounds=s.bounds;lastDivision=division;lastStatus=s.hingeStatus;lastAvailable=s.phoneWithHinge;
            }
            ReMaskedDuo::GameDisplay.Publish(std::move(g));
        }];
    }
    ReMaskedDual::Requested=ReMaskedAppleDuoRequested();
    [Observer refresh];
    if(TouchView&&ObservedView)[ObservedView bringSubviewToFront:TouchView];
}
#else
bool ReMaskedAppleDuoAvailable(){return false;}
bool ReMaskedAppleDuoRequested(){return false;}
void ReMaskedAppleSetDuoRequested(bool){}
void ReMaskedAppleDuoFrame(){}
#endif

void ReMaskedAppleAppendDisplaySettings(Oot3dNativeGame::Menu::Document& document) {
    if(!ReMaskedAppleDuoAvailable())return;
    using namespace Oot3dNativeGame::Menu;
    for(auto& tab:document.Tabs)if(tab.Title=="General") {
        Row row;row.Kind=RowKind::Choice;row.Key="Screen mode";
        row.Help="Dual Screen puts the native lower screen across the iPhone Duo fold. Closing the device returns to Single Screen without changing your preference.";
        row.Choices={"Single Screen","Dual Screen"};row.Minimum=0;row.Maximum=1;row.Value=ReMaskedAppleDuoRequested()?1:0;
        row.Read=[](){return ReMaskedAppleDuoRequested()?1:0;};
        row.Write=[](int v){ReMaskedAppleSetDuoRequested(v>=.5);return WriteResult{};};
        tab.Rows.push_back(std::move(row));break;
    }
}
