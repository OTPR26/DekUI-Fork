// Touch presentation adapted from Linkzenic's Ship of Harkinian iOS port.
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include "touch_controls.h"
#include "portrait_layout.h"
#include "touch_dpad.h"
#if REMASKED_AZAHAR_TOUCH_HOST
#include "AzaharTouchHost.h"
using Sint16 = int16_t;
constexpr auto SDL_MIN_SINT16 = INT16_MIN;
constexpr auto SDL_MAX_SINT16 = INT16_MAX;
#else
#include <SDL.h>
#include "ios_host.h"
#include "three_ds_virtual_pad.h"
#include "three_ds_touch_anchors.h"
#include "three_ds_touch_overlay_settings.h"
#include "android_menu.h"
#include "android_graphics_support.h"
#include "settings_view.h"
#include "mm3d_single_screen.h"
#include "touch_hud_placement.h"
#include "fast/extension/remasked_dual_screen.h"
#include "fast/extension/remasked_duo_layout.h"
#include "apple_duo.h"
#include "fast/extension/graphics_settings_runtime.h"
#include "fast/extension/presentation_picture.h"
#endif

namespace {
void OnTouchMain(dispatch_block_t work){if(NSThread.isMainThread)work();else dispatch_async(dispatch_get_main_queue(),work);}

constexpr int kButtonA = 0;
constexpr int kButtonB = 1;
constexpr int kButtonX = 2;
constexpr int kButtonY = 3;
constexpr int kButtonBack = 4;
constexpr int kButtonStart = 6;
constexpr int kButtonSelect = 7;
constexpr int kButtonLeftShoulder = 9;
constexpr int kButtonRightShoulder = 10;
constexpr int kButtonDpadUp = 11;
constexpr int kButtonDpadDown = 12;
constexpr int kButtonDpadLeft = 13;
constexpr int kButtonDpadRight = 14;
constexpr int kButtonMap = 15;

constexpr int kAxisLeftX = 0;
constexpr int kAxisLeftY = 1;
constexpr int kAxisRightX = 2;
constexpr int kAxisRightY = 3;
constexpr int kAxisLeftTrigger = 4;
constexpr int kAxisRightTrigger = 5;

void SetVirtualButton(int button, bool pressed) { ReMaskedIOSTouchButton(button,pressed); }
void SetVirtualAxis(int axis, Sint16 value) { ReMaskedIOSTouchAxis(axis,value); }

Sint16 ClampAxis(CGFloat value) {
    return static_cast<Sint16>(
        std::clamp(value, static_cast<CGFloat>(SDL_MIN_SINT16),
                   static_cast<CGFloat>(SDL_MAX_SINT16)));
}

#if REMASKED_AZAHAR_TOUCH_HOST
static unsigned sHiddenTouchControls;
static bool sHideWithController;
#endif
static float sIndividualSizes[15]={1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static BOOL sShoulderLabels=NO,sDragLook=NO,sVibration=YES,sPortraitArtwork=NO;
static NSArray<NSString*>* ControlKeys(){return @[@"A",@"B",@"X",@"Y",@"L",@"R",@"ZL",@"ZR",@"Start",@"Select",@"D-pad",@"Move",@"Look",@"Eye",@"Settings"];}
CGFloat sArtworkOpacity=.28;
CGFloat ArtworkAlpha(CGFloat alpha){return MIN(1.0,alpha*sArtworkOpacity/.28);}
UIColor* AndroidControlFill() {
    return [UIColor colorWithWhite:1.0 alpha:ArtworkAlpha(0.125)];
}

UIColor* AndroidControlPressedFill() {
    return [UIColor colorWithWhite:1.0 alpha:ArtworkAlpha(0.063)];
}

UIColor* AndroidControlStroke() {
    return [UIColor colorWithWhite:1.0 alpha:ArtworkAlpha(0.145)];
}

} // namespace

typedef NS_ENUM(NSInteger, ReMaskedButtonShape) {
    ReMaskedButtonShapeCircle,
    ReMaskedButtonShapeRectangle,
    ReMaskedButtonShapeLeftShoulder,
    ReMaskedButtonShapeRightShoulder,
    ReMaskedButtonShapeTransparent,
};

@interface ReMaskedTouchButton : UIButton

@property(nonatomic) int controllerButton;
@property(nonatomic) int controllerAxis;
@property(nonatomic) Sint16 pressedAxisValue;
@property(nonatomic) Sint16 releasedAxisValue;
@property(nonatomic) BOOL inputPressed;
@property(nonatomic, strong) CAShapeLayer* artwork;
@property(nonatomic) ReMaskedButtonShape controlShape;
@property(nonatomic) BOOL hudIcon;

- (instancetype)initWithLabel:(NSString*)label
                       button:(int)button
                        shape:(ReMaskedButtonShape)shape;
- (instancetype)initWithLabel:(NSString*)label
                         axis:(int)axis
                        value:(Sint16)value
                 releaseValue:(Sint16)releaseValue
                        shape:(ReMaskedButtonShape)shape;
- (void)cancelInput;

@end

@implementation ReMaskedTouchButton

- (instancetype)initBaseWithLabel:(NSString*)label shape:(ReMaskedButtonShape)shape {
    self = [super initWithFrame:CGRectZero];
    if (self != nil) {
        self.controllerButton = -1;
        self.controllerAxis = -1;
        self.controlShape = shape;
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.clearColor;
        self.layer.borderColor = AndroidControlStroke().CGColor;
        self.layer.borderWidth = 0;
        self.artwork=[CAShapeLayer layer];
        [self.layer insertSublayer:self.artwork atIndex:0];
        [self setTitle:label forState:UIControlStateNormal];
        [self setTitleColor:AndroidControlStroke() forState:UIControlStateNormal];
        self.titleLabel.font = [UIFont systemFontOfSize:14.0 weight:UIFontWeightSemibold];
        self.accessibilityLabel = label;

        [self addTarget:self
                      action:@selector(inputDown)
            forControlEvents:UIControlEventTouchDown | UIControlEventTouchDragEnter];
        [self addTarget:self
                      action:@selector(inputUp)
            forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                             UIControlEventTouchCancel | UIControlEventTouchDragExit];
    }
    return self;
}

- (instancetype)initWithLabel:(NSString*)label
                       button:(int)button
                        shape:(ReMaskedButtonShape)shape {
    self = [self initBaseWithLabel:label shape:shape];
    if (self != nil) {
        self.controllerButton = button;
    }
    return self;
}

- (instancetype)initWithLabel:(NSString*)label
                         axis:(int)axis
                        value:(Sint16)value
                 releaseValue:(Sint16)releaseValue
                        shape:(ReMaskedButtonShape)shape {
    self = [self initBaseWithLabel:label shape:shape];
    if (self != nil) {
        self.controllerAxis = axis;
        self.pressedAxisValue = value;
        self.releasedAxisValue = releaseValue;
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.titleLabel.font=[UIFont systemFontOfSize:self.hudIcon?10:14 weight:UIFontWeightSemibold];
    if(self.hudIcon)self.titleLabel.frame=CGRectMake(CGRectGetWidth(self.bounds)*.67,CGRectGetHeight(self.bounds)*.66,CGRectGetWidth(self.bounds)*.28,CGRectGetHeight(self.bounds)*.3);
    self.backgroundColor=UIColor.clearColor;
    self.layer.cornerRadius=0;
    self.artwork.frame=self.bounds;
    self.artwork.hidden=self.controlShape==ReMaskedButtonShapeTransparent;
    if(self.artwork.hidden)return;
    CGRect r=CGRectInset(self.bounds,1.25,1.25);
    UIBezierPath* path;
    if(self.controlShape==ReMaskedButtonShapeCircle)path=[UIBezierPath bezierPathWithOvalInRect:r];
    else {
        // Android Ship drawables use a 50dp outer top corner and 10dp
        // corners elsewhere. Normalize adjoining radii to the button height.
        CGFloat tl=10,tr=10,bl=10,br=10;
        if(self.controlShape==ReMaskedButtonShapeLeftShoulder)tl=50;
        if(self.controlShape==ReMaskedButtonShapeRightShoulder)tr=50;
        CGFloat k=MIN(1.0,MIN(CGRectGetWidth(r)/MAX(tl+tr,bl+br),CGRectGetHeight(r)/MAX(tl+bl,tr+br)));
        tl*=k;tr*=k;bl*=k;br*=k;
        CGFloat x=CGRectGetMinX(r),y=CGRectGetMinY(r),w=CGRectGetWidth(r),h=CGRectGetHeight(r);
        path=[UIBezierPath bezierPath];
        [path moveToPoint:CGPointMake(x+tl,y)];[path addLineToPoint:CGPointMake(x+w-tr,y)];
        [path addArcWithCenter:CGPointMake(x+w-tr,y+tr) radius:tr startAngle:-M_PI_2 endAngle:0 clockwise:YES];
        [path addLineToPoint:CGPointMake(x+w,y+h-br)];
        [path addArcWithCenter:CGPointMake(x+w-br,y+h-br) radius:br startAngle:0 endAngle:M_PI_2 clockwise:YES];
        [path addLineToPoint:CGPointMake(x+bl,y+h)];
        [path addArcWithCenter:CGPointMake(x+bl,y+h-bl) radius:bl startAngle:M_PI_2 endAngle:M_PI clockwise:YES];
        [path addLineToPoint:CGPointMake(x,y+tl)];
        [path addArcWithCenter:CGPointMake(x+tl,y+tl) radius:tl startAngle:M_PI endAngle:M_PI*1.5 clockwise:YES];
        [path closePath];
    }
    self.artwork.path=path.CGPath;
    self.artwork.fillColor=(self.controlShape==ReMaskedButtonShapeRectangle?(self.inputPressed?AndroidControlPressedFill():AndroidControlFill()):UIColor.clearColor).CGColor;
    self.artwork.strokeColor=AndroidControlStroke().CGColor;
    self.artwork.lineWidth=2.5;
}

- (void)inputDown {
    if (self.inputPressed) {
        return;
    }
    self.inputPressed = YES;
    self.artwork.fillColor=(self.controlShape==ReMaskedButtonShapeRectangle?AndroidControlPressedFill():UIColor.clearColor).CGColor;
    if (self.controllerButton >= 0) {
        SetVirtualButton(self.controllerButton, true);
    } else if (self.controllerAxis >= 0) {
        SetVirtualAxis(self.controllerAxis, self.pressedAxisValue);
    }
}

- (void)inputUp {
    if (!self.inputPressed) {
        return;
    }
    self.inputPressed = NO;
    self.artwork.fillColor=(self.controlShape==ReMaskedButtonShapeRectangle?AndroidControlFill():UIColor.clearColor).CGColor;
    if (self.controllerButton >= 0) {
        SetVirtualButton(self.controllerButton, false);
    } else if (self.controllerAxis >= 0) {
        SetVirtualAxis(self.controllerAxis, self.releasedAxisValue);
    }
}

- (void)cancelInput {
    [self inputUp];
}

@end

@interface ReMaskedTouchStick : UIView

@property(nonatomic, strong) UIView* base;
@property(nonatomic, strong) UIView* knob;
@property(nonatomic) CGPoint stickCenter;
@property(nonatomic) CGFloat stickDiameter;
@property(nonatomic) int controllerAxisX;
@property(nonatomic) int controllerAxisY;
@property(nonatomic) BOOL floating;
@property(nonatomic) BOOL customPlaced;
@property(nonatomic) BOOL dragLook;
@property(nonatomic) CGPoint lastPoint;
@property(nonatomic) NSTimeInterval lastMove;
@property(nonatomic) NSUInteger dragGeneration;
- (void)cancelInput;

@end

@implementation ReMaskedTouchStick

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        self.multipleTouchEnabled = NO;
        self.backgroundColor = UIColor.clearColor;
        self.accessibilityLabel = @"Floating left joystick";
        self.controllerAxisX = kAxisLeftX;
        self.controllerAxisY = kAxisLeftY;
        _floating = YES;

        self.base = [[UIView alloc] initWithFrame:CGRectZero];
        self.base.userInteractionEnabled = NO;
        self.base.backgroundColor = UIColor.clearColor;
        self.base.layer.borderColor = AndroidControlStroke().CGColor;
        self.base.layer.borderWidth = 2.0;
        self.base.hidden = YES;
        [self addSubview:self.base];

        self.knob = [[UIView alloc] initWithFrame:CGRectZero];
        self.knob.userInteractionEnabled = NO;
        self.knob.backgroundColor = UIColor.clearColor;
        self.knob.layer.borderColor = AndroidControlStroke().CGColor;
        self.knob.layer.borderWidth = 2.5;
        [self.base addSubview:self.knob];
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat diameter = self.stickDiameter > 0.0 ? self.stickDiameter : 150.0;
    self.base.bounds = CGRectMake(0.0, 0.0, diameter, diameter);
    self.base.layer.cornerRadius = diameter * 0.5;
    CGFloat knobSize = diameter * 0.32;
    self.knob.bounds = CGRectMake(0.0, 0.0, knobSize, knobSize);
    self.knob.layer.cornerRadius = knobSize * 0.5;
    self.knob.center = CGPointMake(diameter * 0.5, diameter * 0.5);
    if (!self.floating) {
        self.stickCenter = self.customPlaced ? CGPointMake(CGRectGetMidX(self.bounds),CGRectGetMidY(self.bounds)) : CGPointMake(MAX(diameter * 0.58, CGRectGetWidth(self.bounds) * 0.24),
                                       CGRectGetHeight(self.bounds) - diameter * 0.58);
        self.base.center = self.stickCenter;
        self.base.hidden = NO;
    }
}

- (void)updateForPoint:(CGPoint)point {
    if(self.dragLook){
        NSTimeInterval now=NSProcessInfo.processInfo.systemUptime,seconds=MAX(.008,now-self.lastMove);
        SetVirtualAxis(self.controllerAxisX,ClampAxis((point.x-self.lastPoint.x)/seconds/600.0*SDL_MAX_SINT16));
        SetVirtualAxis(self.controllerAxisY,ClampAxis((point.y-self.lastPoint.y)/seconds/600.0*SDL_MAX_SINT16));
        self.lastPoint=point;self.lastMove=now;
        NSUInteger generation=++self.dragGeneration;
        __weak ReMaskedTouchStick* weakSelf=self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,50*NSEC_PER_MSEC),dispatch_get_main_queue(),^{
            ReMaskedTouchStick* stick=weakSelf;
            if(stick&&stick.dragGeneration==generation){SetVirtualAxis(stick.controllerAxisX,0);SetVirtualAxis(stick.controllerAxisY,0);}
        });return;
    }
    CGPoint center = self.stickCenter;
    CGFloat knobRadius = CGRectGetWidth(self.knob.bounds) * 0.5;
    CGFloat radius = self.stickDiameter * 0.5 - knobRadius;
    CGFloat dx = point.x - center.x;
    CGFloat dy = point.y - center.y;
    CGFloat distance = hypot(dx, dy);
    if (distance > radius && distance > 0.0) {
        dx = dx / distance * radius;
        dy = dy / distance * radius;
    }

    self.knob.center =
        CGPointMake(self.stickDiameter * 0.5 + dx, self.stickDiameter * 0.5 + dy);
    SetVirtualAxis(self.controllerAxisX, ClampAxis((dx / radius) * SDL_MAX_SINT16));
    SetVirtualAxis(self.controllerAxisY, ClampAxis((dy / radius) * SDL_MAX_SINT16));
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    if (touch != nil) {
        CGPoint point = [touch locationInView:self];
        self.lastPoint=point;self.lastMove=NSProcessInfo.processInfo.systemUptime;
        if (self.floating) {
            self.stickCenter = point;
            self.base.center = self.stickCenter;
        }
        self.base.hidden = self.dragLook;
        [self updateForPoint:point];
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = touches.anyObject;
    if (touch != nil) {
        [self updateForPoint:[touch locationInView:self]];
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self cancelInput];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self cancelInput];
}

- (void)cancelInput {
    ++self.dragGeneration;
    SetVirtualAxis(self.controllerAxisX, 0);
    SetVirtualAxis(self.controllerAxisY, 0);
    self.knob.center =
        CGPointMake(self.stickDiameter * 0.5, self.stickDiameter * 0.5);
    self.base.hidden = self.floating || self.dragLook;
}

- (void)setFloating:(BOOL)floating {
    _floating = floating;
    [self cancelInput];
    [self setNeedsLayout];
}

@end

@interface ReMaskedCrossView : UIView
@property(nonatomic) BOOL hudIcons;
@end

@implementation ReMaskedCrossView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        self.userInteractionEnabled = NO;
        self.backgroundColor = UIColor.clearColor;
    }
    return self;
}

- (void)drawRect:(CGRect)rect {
    CGContextRef context=UIGraphicsGetCurrentContext();
    CGContextSaveGState(context);
    CGContextScaleCTM(context,CGRectGetWidth(rect)/221.78,CGRectGetHeight(rect)/221.78);
    CGContextSetLineWidth(context,2.5);
    for(unsigned i=0;i<(self.hudIcons?1u:5u);++i) {
        CGPathRef path=ReMaskedDpadPath(i);
        CGContextAddPath(context,path);
        CGContextSetFillColorWithColor(context,(i==0?AndroidControlFill():AndroidControlStroke()).CGColor);
        CGContextSetStrokeColorWithColor(context,AndroidControlStroke().CGColor);
        CGContextDrawPath(context,i==0?(kCGPathStroke):kCGPathFill);
        CGPathRelease(path);
    }
    CGContextRestoreGState(context);
}

@end

@class ReMaskedTouchLayoutEditor;
@interface ReMaskedTouchOverlay : UIView

@property(nonatomic, strong) UIView* controlGroup;
@property(nonatomic, strong) ReMaskedTouchStick* leftStick;
@property(nonatomic, strong) ReMaskedTouchStick* rightStick;
@property(nonatomic, strong) ReMaskedCrossView* cCross;
@property(nonatomic, strong) ReMaskedTouchButton* buttonA;
@property(nonatomic, strong) ReMaskedTouchButton* buttonB;
@property(nonatomic, strong) ReMaskedTouchButton* buttonX;
@property(nonatomic, strong) ReMaskedTouchButton* buttonY;
@property(nonatomic, strong) ReMaskedTouchButton* buttonL;
@property(nonatomic, strong) ReMaskedTouchButton* buttonZL;
@property(nonatomic, strong) ReMaskedTouchButton* buttonZR;
@property(nonatomic, strong) ReMaskedTouchButton* buttonR;
@property(nonatomic, strong) ReMaskedTouchButton* buttonBack;
@property(nonatomic, strong) ReMaskedTouchButton* buttonStart;
@property(nonatomic, strong) ReMaskedTouchButton* buttonSelect;
@property(nonatomic, strong) ReMaskedTouchButton* buttonMap;
#if REMASKED_AZAHAR_TOUCH_HOST
@property(nonatomic, strong) ReMaskedTouchButton* buttonPictograph;
@property(nonatomic) BOOL airPlaySideControls;
#endif
@property(nonatomic, strong) ReMaskedTouchLayoutEditor* layoutEditor;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSArray<NSNumber*>*>* placements;
@property(nonatomic) BOOL dualLayout;
@property(nonatomic) BOOL portraitLayout;
@property(nonatomic, strong) NSMutableDictionary<NSString*, NSNumber*>* sizes;
- (CGFloat)sizeForControl:(NSString*)key;
- (NSDictionary<NSString*, UIView*>*)placementControls;
- (CGRect)placementRect:(NSString*)key;
- (void)startLayoutEditing;
- (void)loadPlacements;
- (void)applyPlacements;
- (void)finishLayoutEditing:(BOOL)save;
@property(nonatomic, strong) NSArray<ReMaskedTouchButton*>* cButtons;
@property(nonatomic, strong) NSArray<ReMaskedTouchButton*>* allInputButtons;
@property(nonatomic, strong) UIButton* visibilityButton;
@property(nonatomic) BOOL controlsHiddenByUser;
@property(nonatomic) BOOL controlsEnabled;
@property(nonatomic) BOOL menuVisible;
@property(nonatomic) CGFloat controlScale;
@property(nonatomic) BOOL extraShoulders;
@property(nonatomic) NSInteger manualJump;
@property(nonatomic) BOOL hudIcons;
@property(nonatomic) NSInteger faceButtonLayout;

- (void)cancelAllInputs;
- (void)setReMaskedFaceButtonLayout:(NSInteger)layout;
- (void)setReMaskedMenuVisible:(BOOL)visible;

@end


#include "touch_layout_editor.mm.inc"

static void ReMaskedIOSPublishAnchors(ReMaskedTouchOverlay* overlay);
@implementation ReMaskedTouchOverlay

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        self.backgroundColor = UIColor.clearColor;
        self.multipleTouchEnabled = YES;
        self.controlsEnabled = YES;
        self.faceButtonLayout = 0;
        self.controlScale = 1;
        [self loadPlacements];

        self.controlGroup = [[UIView alloc] initWithFrame:self.bounds];
        self.controlGroup.backgroundColor = UIColor.clearColor;
        self.controlGroup.multipleTouchEnabled = YES;
        self.controlGroup.autoresizingMask =
            UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [self addSubview:self.controlGroup];

        self.leftStick = [[ReMaskedTouchStick alloc] initWithFrame:CGRectZero];
        self.rightStick = [[ReMaskedTouchStick alloc] initWithFrame:CGRectZero];
        self.rightStick.controllerAxisX = kAxisRightX;
        self.rightStick.controllerAxisY = kAxisRightY;
        self.rightStick.accessibilityLabel = @"Floating right joystick";
        self.cCross = [[ReMaskedCrossView alloc] initWithFrame:CGRectZero];

        self.buttonA = [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                            button:kButtonA
                                                             shape:ReMaskedButtonShapeCircle];
        self.buttonB = [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                            button:kButtonB
                                                             shape:ReMaskedButtonShapeCircle];
        self.buttonX = [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                            button:kButtonX
                                                             shape:ReMaskedButtonShapeCircle];
        self.buttonY = [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                            button:kButtonY
                                                             shape:ReMaskedButtonShapeCircle];
        self.buttonL =
            [[ReMaskedTouchButton alloc] initWithLabel:@"L"
                                                 button:kButtonLeftShoulder
                                                  shape:ReMaskedButtonShapeLeftShoulder];
        self.buttonZL =
            [[ReMaskedTouchButton alloc] initWithLabel:@"ZL"
                                                   axis:kAxisLeftTrigger
                                                  value:SDL_MAX_SINT16
                                           releaseValue:SDL_MIN_SINT16
                                                  shape:ReMaskedButtonShapeLeftShoulder];
        self.buttonZR =
            [[ReMaskedTouchButton alloc] initWithLabel:@"ZR"
                                                   axis:kAxisRightTrigger
                                                  value:SDL_MAX_SINT16
                                           releaseValue:SDL_MIN_SINT16
                                                  shape:ReMaskedButtonShapeRightShoulder];
        self.buttonR =
            [[ReMaskedTouchButton alloc] initWithLabel:@"R"
                                                 button:kButtonRightShoulder
                                                  shape:ReMaskedButtonShapeRightShoulder];
        self.buttonBack =
            [[ReMaskedTouchButton alloc] initWithLabel:@"Settings"
                                                 button:kButtonBack
                                                  shape:ReMaskedButtonShapeRectangle];
        self.buttonStart =
            [[ReMaskedTouchButton alloc] initWithLabel:@"Start"
                                                 button:kButtonStart
                                                  shape:ReMaskedButtonShapeRectangle];

        self.buttonSelect=[[ReMaskedTouchButton alloc] initWithLabel:@"Select" button:kButtonSelect shape:ReMaskedButtonShapeRectangle];
        ReMaskedTouchButton* cUp =
            [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                 button:kButtonDpadUp
                                                  shape:ReMaskedButtonShapeTransparent];
        ReMaskedTouchButton* cDown =
            [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                 button:kButtonDpadDown
                                                  shape:ReMaskedButtonShapeTransparent];
        ReMaskedTouchButton* cLeft =
            [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                 button:kButtonDpadLeft
                                                  shape:ReMaskedButtonShapeTransparent];
        ReMaskedTouchButton* cRight =
            [[ReMaskedTouchButton alloc] initWithLabel:@""
                                                 button:kButtonDpadRight
                                                  shape:ReMaskedButtonShapeTransparent];
        for (ReMaskedTouchButton* button in @[ cUp, cDown, cLeft, cRight ]) {
            button.backgroundColor = UIColor.clearColor;
            button.layer.borderWidth = 0.0;
        }
        cUp.accessibilityLabel = @"D-pad up";
        cDown.accessibilityLabel = @"D-pad down";
        cLeft.accessibilityLabel = @"D-pad left";
        cRight.accessibilityLabel = @"D-pad right";
        self.cButtons = @[ cUp, cDown, cLeft, cRight ];

        [self.controlGroup addSubview:self.leftStick];
        [self.controlGroup addSubview:self.rightStick];
        [self.controlGroup addSubview:self.cCross];

        self.buttonMap = [[ReMaskedTouchButton alloc] initWithLabel:@"" button:kButtonMap shape:ReMaskedButtonShapeCircle];
        [self.buttonMap setImage:[UIImage systemImageNamed:@"map"] forState:UIControlStateNormal];
        self.buttonMap.tintColor=AndroidControlStroke();
        self.buttonMap.accessibilityLabel=@"Toggle minimap";
        self.allInputButtons = @[self.buttonMap,
            self.buttonA, self.buttonB, self.buttonX, self.buttonY, self.buttonL,
            self.buttonZL, self.buttonZR, self.buttonR, self.buttonBack, self.buttonStart, self.buttonSelect,
            cUp, cDown, cLeft, cRight,
        ];
#if REMASKED_AZAHAR_TOUCH_HOST
        self.buttonPictograph = [[ReMaskedTouchButton alloc] initWithLabel:@""
            button:ReMaskedTouchPictograph shape:ReMaskedButtonShapeCircle];
        [self.buttonPictograph setImage:[UIImage systemImageNamed:@"camera"] forState:UIControlStateNormal];
        self.buttonPictograph.tintColor = AndroidControlStroke();
        self.buttonPictograph.accessibilityLabel = @"Pictograph Box";
        self.allInputButtons = [self.allInputButtons arrayByAddingObject:self.buttonPictograph];
#endif
        for (ReMaskedTouchButton* button in self.allInputButtons) {
            [self.controlGroup addSubview:button];
        }


        self.visibilityButton = [UIButton buttonWithType:UIButtonTypeCustom];
        self.visibilityButton.backgroundColor = UIColor.clearColor;
        [self.visibilityButton setTitle:@"◉" forState:UIControlStateNormal];
#if REMASKED_AZAHAR_TOUCH_HOST
        // Keep the recovery control legible even at zero gameplay control opacity.
        [self.visibilityButton setTitleColor:[UIColor colorWithWhite:1.0 alpha:.75]
                                    forState:UIControlStateNormal];
#else
        [self.visibilityButton setTitleColor:AndroidControlFill() forState:UIControlStateNormal];
#endif
        self.visibilityButton.titleLabel.font =
            [UIFont systemFontOfSize:22.0 weight:UIFontWeightRegular];
        self.visibilityButton.accessibilityLabel = @"Show or hide touch controls";
        [self.visibilityButton addTarget:self
                                  action:@selector(toggleControlsVisibility)
                        forControlEvents:UIControlEventTouchUpInside];
        [self addSubview:self.visibilityButton];
#if REMASKED_AZAHAR_TOUCH_HOST
        NSNotificationCenter* notifications = NSNotificationCenter.defaultCenter;
        for (NSString* name in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification]) {
            [notifications addObserver:self selector:@selector(gameplayControllerChanged:)
                                  name:name object:nil];
        }
#endif
    }
    return self;
}

- (NSString*)placementPreference {
    return self.portraitLayout?@"ReMasked.TouchLayout.Portrait.v1":(self.dualLayout?@"ReMasked.TouchLayout.Dual.v1":@"ReMasked.TouchLayout.Single.v1");
}
- (CGFloat)sizeForControl:(NSString*)key {
    NSUInteger index=[ControlKeys() indexOfObject:key];
#if REMASKED_AZAHAR_TOUCH_HOST
    if ([key isEqualToString:@"Settings"]) index = [ControlKeys() indexOfObject:@"Select"];
#endif
    return self.sizes[key]?self.sizes[key].doubleValue:(index<15?sIndividualSizes[index]:1);
}
- (void)loadPlacements {
    self.sizes=[NSMutableDictionary dictionary];
    id sizes=[NSUserDefaults.standardUserDefaults objectForKey:[[self placementPreference] stringByAppendingString:@".sizes"]];
    if([sizes isKindOfClass:NSDictionary.class]&&[sizes count]<=15)for(NSString* key in ControlKeys()){
        id value=sizes[key];if([value isKindOfClass:NSNumber.class]&&std::isfinite([value doubleValue]))self.sizes[key]=@(std::clamp([value doubleValue],.5,2.));
    }
    self.placements=[NSMutableDictionary dictionary];
    id saved=[NSUserDefaults.standardUserDefaults objectForKey:[self placementPreference]];
    if(![saved isKindOfClass:NSDictionary.class] || [saved count]>16)return;
    NSSet* allowed=[NSSet setWithArray:@[@"A",@"B",@"X",@"Y",@"L",@"R",@"ZL",@"ZR",@"Settings",@"Start",@"Select",@"Map",@"D-pad",@"Move",@"Look",@"Eye"]];
    for(id key in saved) {
        id pair=saved[key];
        if(![key isKindOfClass:NSString.class] || ![allowed containsObject:key] ||
           ![pair isKindOfClass:NSArray.class] || [pair count]!=2 ||
           ![pair[0] isKindOfClass:NSNumber.class] || ![pair[1] isKindOfClass:NSNumber.class])continue;
        double x=[pair[0] doubleValue],y=[pair[1] doubleValue];
        if(std::isfinite(x)&&std::isfinite(y))self.placements[key]=@[@(std::clamp(x,0.,1.)),@(std::clamp(y,0.,1.))];
    }
}
- (NSDictionary<NSString*, UIView*>*)placementControls {
    return @{@"A":self.buttonA,@"B":self.buttonB,@"X":self.buttonX,@"Y":self.buttonY,
        @"L":self.buttonL,@"R":self.buttonR,@"ZL":self.buttonZL,@"ZR":self.buttonZR,
        @"Settings":self.buttonBack,@"Start":self.buttonStart,@"Map":self.buttonMap,
        @"D-pad":self.cCross,@"Move":self.leftStick,@"Look":self.rightStick,@"Eye":self.visibilityButton};
}
- (CGRect)placementRect:(NSString*)key {
    UIView* control=[self placementControls][key];
    CGRect rect=[control.superview convertRect:control.frame toView:self];
    if(([key isEqual:@"Move"]||[key isEqual:@"Look"])&&!self.placements[key]) {
        ReMaskedTouchStick* stick=(ReMaskedTouchStick*)control;CGFloat size=stick.stickDiameter;
        BOOL move=[key isEqual:@"Move"];
        rect=CGRectMake(move?self.safeAreaInsets.left+16:self.bounds.size.width*.62-size/2,
            move?self.bounds.size.height-self.safeAreaInsets.bottom-size-16:self.bounds.size.height*.62-size/2,size,size);
    }
    return rect;
}
- (void)applyPlacements {
    self.leftStick.customPlaced=self.placements[@"Move"]!=nil;
    self.rightStick.customPlaced=self.placements[@"Look"]!=nil;
    NSDictionary* controls=[self placementControls];
    NSArray* keys=ControlKeys();
    for(NSUInteger i=0;i<keys.count;++i){
        UIView* view=controls[keys[i]];if(!view)continue;CGFloat factor=[self sizeForControl:keys[i]];
        if([view isKindOfClass:ReMaskedTouchStick.class]){((ReMaskedTouchStick*)view).stickDiameter*=factor;continue;}
        CGPoint center=view.center;CGSize size=view.bounds.size;
        view.bounds=CGRectMake(0,0,size.width*factor,size.height*factor);view.center=center;
        if(view==self.cCross)for(UIView* button in self.cButtons){
            CGPoint old=button.center;CGSize b=button.bounds.size;
            button.bounds=CGRectMake(0,0,b.width*factor,b.height*factor);
            button.center=CGPointMake(center.x+(old.x-center.x)*factor,center.y+(old.y-center.y)*factor);
        }
        UIEdgeInsets safe=self.safeAreaInsets;
        CGPoint parent=[view.superview convertPoint:view.center toView:self];
        CGFloat x=std::clamp(parent.x,safe.left+view.bounds.size.width/2,MAX(safe.left+view.bounds.size.width/2,self.bounds.size.width-safe.right-view.bounds.size.width/2));
        CGFloat y=std::clamp(parent.y,safe.top+view.bounds.size.height/2,MAX(safe.top+view.bounds.size.height/2,self.bounds.size.height-safe.bottom-view.bounds.size.height/2));
        if(factor!=1){view.center=[self convertPoint:CGPointMake(x,y) toView:view.superview];
            if(view==self.cCross)for(UIView* button in self.cButtons)button.center=CGPointMake(button.center.x+x-parent.x,button.center.y+y-parent.y);}
    }
    self.rightStick.dragLook=sDragLook;
    for(NSString* key in self.placements) {
        UIView* view=controls[key];if(!view || (view==self.rightStick&&sDragLook&&!self.layoutEditor))continue;
        NSArray* pair=self.placements[key];CGFloat w=self.bounds.size.width,h=self.bounds.size.height;
        CGSize size=view.bounds.size;
        if([view isKindOfClass:ReMaskedTouchStick.class])size=CGSizeMake(((ReMaskedTouchStick*)view).stickDiameter,((ReMaskedTouchStick*)view).stickDiameter);
        UIEdgeInsets safe=self.safeAreaInsets;
        if(self.portraitLayout){safe.top=CGRectGetMinY(self.controlGroup.frame);safe.bottom=h-CGRectGetMaxY(self.controlGroup.frame);safe.left=CGRectGetMinX(self.controlGroup.frame);safe.right=w-CGRectGetMaxX(self.controlGroup.frame);}
        CGFloat x=std::clamp([pair[0] doubleValue]*w,safe.left+size.width/2,MAX(safe.left+size.width/2,w-safe.right-size.width/2));
        CGFloat y=std::clamp([pair[1] doubleValue]*h,safe.top+size.height/2,MAX(safe.top+size.height/2,h-safe.bottom-size.height/2));
        CGPoint target=[self convertPoint:CGPointMake(x,y) toView:view.superview];
        CGPoint delta=CGPointMake(target.x-view.center.x,target.y-view.center.y);
        view.bounds=CGRectMake(0,0,size.width,size.height);view.center=target;
        if(view==self.cCross)for(UIView* button in self.cButtons)button.center=CGPointMake(button.center.x+delta.x,button.center.y+delta.y);
        if([view isKindOfClass:ReMaskedTouchStick.class]){
            ReMaskedTouchStick* stick=(ReMaskedTouchStick*)view;
            stick.stickCenter=CGPointMake(size.width/2,size.height/2);stick.base.center=stick.stickCenter;
        }
    }
}
- (void)startLayoutEditing {
    if(self.layoutEditor)return;
    [self cancelAllInputs];[self layoutIfNeeded];
    self.layoutEditor=[[ReMaskedTouchLayoutEditor alloc] initWithOwner:self];
    self.hidden=NO;[self addSubview:self.layoutEditor];
}
- (void)finishLayoutEditing:(BOOL)save {
    if(!self.layoutEditor)return;
    if(save){
        [NSUserDefaults.standardUserDefaults setObject:self.placements forKey:[self placementPreference]];
        [NSUserDefaults.standardUserDefaults setObject:self.sizes forKey:[[self placementPreference] stringByAppendingString:@".sizes"]];
    }
    else [self loadPlacements];
    [self.layoutEditor removeFromSuperview];self.layoutEditor=nil;
    self.hidden=self.menuVisible;[self setNeedsLayout];
}

- (void)setReMaskedFaceButtonLayout:(NSInteger)layout {
    self.faceButtonLayout = std::clamp<NSInteger>(layout, 0, 2);

    self.buttonA.controllerButton=kButtonA;
    self.buttonB.controllerButton=kButtonB;
    self.buttonX.controllerButton=kButtonX;
    self.buttonY.controllerButton=kButtonY;
    [self setNeedsLayout];
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
#if REMASKED_AZAHAR_TOUCH_HOST
    const CGRect screen = ReMaskedAzaharBottomPicture();
    if (!CGRectIsNull(screen) && CGRectContainsPoint(screen, [self convertPoint:point toView:self.window]))
        return nil;
#else
    if(!self.layoutEditor&&ReMaskedDuo::GameDisplay.MapTouch(point.x,point.y,ReMaskedDual::Active.load()))return nil;
#endif
    UIView* hit = [super hitTest:point withEvent:event];
    if (self.layoutEditor) return hit ?: self.layoutEditor;
    if (hit == self || hit == self.controlGroup) {
        return nil;
    }
    return hit;
}

#if REMASKED_AZAHAR_TOUCH_HOST
- (void)gameplayControllerChanged:(NSNotification*)notification {
    OnTouchMain(^{ [self applyVisibility]; });
}
#endif

- (void)toggleControlsVisibility {
#if !REMASKED_AZAHAR_TOUCH_HOST
    if (!self.controlsEnabled) return;
#endif
    self.controlsHiddenByUser = !self.controlsHiddenByUser;
    [self cancelAllInputs];
#if REMASKED_AZAHAR_TOUCH_HOST
    ReMaskedAzaharReleaseTouch();
#else
    ThreeDsRecomp::Input::TouchOverlayPad().ReleaseAll();
#endif
    [self applyVisibility];
}

- (void)applyVisibility {
#if REMASKED_AZAHAR_TOUCH_HOST
    // The Settings switch supplies the initial state. The in-game button
    // can reveal the controls even when that preference is off.
    BOOL hideGameplayControls = self.controlsHiddenByUser;
    BOOL hasGamepad = NO;
    for (GCController* controller in GCController.controllers) {
        if (controller.extendedGamepad) {
            hasGamepad = YES;
            break;
        }
    }
    hideGameplayControls |= sHideWithController && hasGamepad && !self.layoutEditor;
    self.visibilityButton.hidden = hasGamepad;
    self.visibilityButton.accessibilityLabel = hideGameplayControls ? @"Show touch controls" : @"Hide touch controls";
#else
    BOOL hideGameplayControls = !self.controlsEnabled || self.controlsHiddenByUser;
    self.visibilityButton.hidden = !self.controlsEnabled;
#endif
    for (UIView* view in self.controlGroup.subviews) {
        view.hidden = hideGameplayControls;
    }
    self.buttonZL.hidden=hideGameplayControls || !(self.extraShoulders || self.manualJump!=0);
    self.buttonZR.hidden=hideGameplayControls || !(self.extraShoulders || self.manualJump!=0);
    self.buttonBack.hidden = hideGameplayControls;
    self.buttonMap.hidden=YES;
    self.buttonSelect.hidden=YES;
#if REMASKED_AZAHAR_TOUCH_HOST
    if (!self.layoutEditor) {
        NSArray<UIView*>* ordered = @[self.buttonA,self.buttonB,self.buttonX,self.buttonY,
            self.buttonL,self.buttonR,self.buttonZL,self.buttonZR,self.buttonStart,self.buttonBack,
            self.cCross,self.leftStick,self.rightStick];
        for (unsigned i = 0; i < ordered.count; ++i) {
            // Preserve touch access to Settings when no gamepad is connected.
            if (i == 9 && !hasGamepad) continue;
            if (sHiddenTouchControls & (1u << i)) ordered[i].hidden = YES;
        }
        if (sHiddenTouchControls & (1u << 10))
            for (UIView* direction in self.cButtons) direction.hidden = YES;
    }
    ReMaskedIOSPublishAnchors(self);
#endif
}

- (void)setReMaskedMenuVisible:(BOOL)visible {
    if (self.menuVisible == visible) {
        return;
    }
    if(!visible && self.layoutEditor)[self finishLayoutEditing:NO];
    self.menuVisible = visible;
    [self setNeedsLayout];
    [self cancelAllInputs];
    [self applyVisibility];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    UIEdgeInsets safe = self.safeAreaInsets;
    CGFloat width = CGRectGetWidth(self.bounds),height=CGRectGetHeight(self.bounds);
#if !REMASKED_AZAHAR_TOUCH_HOST
    auto panes=ReMaskedDuo::GameDisplay.Pixels(width,height,self.dualLayout);
#endif
    CGRect content=self.bounds;
    const auto portrait=ReMaskedIOS::PhonePortrait.Pixels(width,height);
    if(self.portraitLayout!=portrait.Active()){
        [self cancelAllInputs];self.portraitLayout=portrait.Active();[self loadPlacements];
    }
    if(self.portraitLayout){
        const auto p=portrait.Controls;content=CGRectMake(p.X,p.Y,p.Width,p.Height);safe=UIEdgeInsetsZero;
#if REMASKED_AZAHAR_TOUCH_HOST
    }else{
        const CGRect safeFrame=[self convertRect:UIEdgeInsetsInsetRect(self.bounds,safe) toView:self.window];
        content=[self convertRect:ReMaskedAzaharTouchCanvas(safeFrame) fromView:self.window];
        safe=UIEdgeInsetsZero;
#else
    }else if(self.dualLayout&&panes.Dual()){
        content=CGRectMake(panes.Top.X,panes.Top.Y,panes.Top.Width,panes.Top.Height);safe=UIEdgeInsetsZero;
    }else{
        // Native HUD artwork is fitted into the presentation picture, then its
        // 16:9 MM canvas. UIKit must use that same canvas, not the wider window.
        const CGFloat density=self.window.screen.scale ?: UIScreen.mainScreen.scale;
        const auto picture=Fast::Extension::ResolvePresentationPicture(
            uint32_t(std::lround(width*density)),uint32_t(std::lround(height*density)),
            Fast::Extension::GraphicsSettingsRuntime::Instance().Snapshot().Aspect);
        const auto hud=Oot3d::Renderer::FitUiPresentationViewport(picture.Width,picture.Height,16.f/9.f,1.f);
        if(hud.Valid())content=CGRectMake((picture.X+hud.X)/density,(picture.Y+hud.Y)/density,hud.Width/density,hud.Height/density);
        safe=UIEdgeInsetsMake(MAX(0,safe.top-CGRectGetMinY(content)),MAX(0,safe.left-CGRectGetMinX(content)),
            MAX(0,safe.bottom-(height-CGRectGetMaxY(content))),MAX(0,safe.right-(width-CGRectGetMaxX(content))));
#endif
    }
    bool splitControls = false;
    bool airPlayBottom = false;
    CGFloat leftRail = 0, rightRail = 0;
#if REMASKED_AZAHAR_TOUCH_HOST
    const CGRect bottomPicture = ReMaskedAzaharBottomPicture();
    airPlayBottom = !CGRectIsNull(bottomPicture);
    if (airPlayBottom && !self.portraitLayout) {
        const CGRect overlayPicture = [self convertRect:bottomPicture fromView:self.window];
        leftRail = CGRectGetMinX(overlayPicture) - CGRectGetMinX(content);
        rightRail = CGRectGetMaxX(content) - CGRectGetMaxX(overlayPicture);
        splitControls = leftRail > 0 && rightRail > 0;
        if (splitControls && content.size.height > 420)
            content = CGRectInset(content, 0, (content.size.height - 420)/2);
    }
    if (self.airPlaySideControls != splitControls) {
        [self cancelAllInputs];
        self.airPlaySideControls = splitControls;
    }
#endif
    self.controlGroup.frame=content;width=content.size.width;height=content.size.height;
    BOOL isPad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    CGFloat maximumScale = 1.35;
    CGFloat scale = self.controlScale * (self.portraitLayout?
        std::clamp(width/440.0,0.82,1.15):std::clamp(height/390.0,0.82,maximumScale));
    if (splitControls) scale = std::min({scale, std::min(leftRail,rightRail)/148.0, height/440.0});
    const auto portraitRows=ReMaskedIOS::ResolvePortraitControlRows(height,scale);
    CGFloat left = safe.left + 16.0 * scale;
    CGFloat right = safe.right + 16.0 * scale;
    CGFloat top = safe.top + 16.0 * scale;
    CGFloat bottom = safe.bottom + 16.0 * scale;
    // AirPlay side controls share two thumb rows across the bottom-screen picture.
    const CGFloat upperThumbY = 182.0 * scale;
    const CGFloat lowerThumbY = height - bottom - 128.0 * scale;

    CGFloat shoulderWidth = 63.0 * scale;
    CGFloat shoulderHeight = 34.5 * scale;
    CGFloat shoulderGap = 10.0 * scale;
    if (splitControls) {
        left = (leftRail - shoulderWidth) / 2;
        right = (rightRail - shoulderWidth) / 2;
    }
    self.buttonZL.frame = CGRectMake(left, top, shoulderWidth, shoulderHeight);
    self.buttonL.frame =
        CGRectMake(left, ((self.extraShoulders || self.manualJump!=0)?CGRectGetMaxY(self.buttonZL.frame) + shoulderGap:top),
                   shoulderWidth, shoulderHeight);
    self.buttonZR.frame =
        CGRectMake(width - right - shoulderWidth, top, shoulderWidth, shoulderHeight);
    self.buttonR.frame =
        CGRectMake(width - right - shoulderWidth,
                   ((self.extraShoulders || self.manualJump!=0)?CGRectGetMaxY(self.buttonZR.frame) + shoulderGap:top),
                   shoulderWidth, shoulderHeight);

    CGFloat stickSize = (splitControls ? 112.0 : 150.0) * scale;
    self.leftStick.stickDiameter = stickSize;
    self.leftStick.frame = CGRectMake(0.0, 0.0, width * 0.46, height);
    if (splitControls) {
        self.leftStick.frame = CGRectMake(0, lowerThumbY-stickSize/2, leftRail, stickSize);
    }
    if(self.menuVisible)self.leftStick.frame=CGRectMake(left,height-bottom-stickSize,stickSize,stickSize);
    CGFloat rightStickSize = (splitControls ? 112.0 : 150.0) * scale;
    self.rightStick.stickDiameter = rightStickSize;
    CGFloat rightStickStart = width * 0.52;
    self.rightStick.frame =
        CGRectMake(rightStickStart, 0.0, width - rightStickStart, height);
    if (splitControls) {
        const CGFloat stickTop = upperThumbY + 74.0 * scale;
        self.rightStick.frame = CGRectMake(width-rightRail,stickTop,rightRail,
                                           height-bottom-38.0*scale-stickTop);
    }

    self.rightStick.userInteractionEnabled=!self.menuVisible;
    CGFloat crossSize = 96.0 * scale;
    self.cCross.frame =
        CGRectMake(left,
                   (self.portraitLayout?portraitRows.ThumbCenter-crossSize/2:MAX(top+114*scale,height-bottom-252*scale)),
                   crossSize, crossSize);
    if (splitControls)
        self.cCross.frame = CGRectMake((leftRail-crossSize)/2,upperThumbY-crossSize/2,crossSize,crossSize);

    CGFloat cHit = crossSize/3;
    CGFloat crossMidX = CGRectGetMidX(self.cCross.frame);
    CGFloat crossMidY = CGRectGetMidY(self.cCross.frame);
    self.cButtons[0].frame =
        CGRectMake(crossMidX - cHit * 0.5, CGRectGetMinY(self.cCross.frame), cHit, cHit);
    self.cButtons[1].frame =
        CGRectMake(crossMidX - cHit * 0.5, CGRectGetMaxY(self.cCross.frame) - cHit, cHit, cHit);
    self.cButtons[2].frame =
        CGRectMake(CGRectGetMinX(self.cCross.frame), crossMidY - cHit * 0.5, cHit, cHit);
    self.cButtons[3].frame =
        CGRectMake(CGRectGetMaxX(self.cCross.frame) - cHit, crossMidY - cHit * 0.5, cHit, cHit);

    CGFloat faceSize = 44.0 * scale;
    CGFloat faceOffset = 44.0 * scale;
    CGFloat faceCenterX = width - right - 93*scale;
    CGFloat faceCenterY = self.portraitLayout?portraitRows.ThumbCenter:height-bottom-93*scale;
    if (splitControls) {
        faceCenterX = width - rightRail/2;
        faceCenterY = upperThumbY;
    }
    for (ReMaskedTouchButton* button in
         @[ self.buttonA, self.buttonB, self.buttonX, self.buttonY ]) {
        button.bounds = CGRectMake(0.0, 0.0, faceSize, faceSize);
    }
    if (self.faceButtonLayout == 0) {
        // Nintendo: X top, Y left, A right, B bottom.
        self.buttonX.center = CGPointMake(faceCenterX, faceCenterY - faceOffset);
        self.buttonY.center = CGPointMake(faceCenterX - faceOffset, faceCenterY);
        self.buttonA.center = CGPointMake(faceCenterX + faceOffset, faceCenterY);
        self.buttonB.center = CGPointMake(faceCenterX, faceCenterY + faceOffset);
    } else if (self.faceButtonLayout == 2) {
        // GameCube: prominent A, B lower-left, X upper-right, Y upper-center.
        self.buttonA.bounds = CGRectMake(0.0, 0.0, faceSize * 1.35, faceSize * 1.35);
        self.buttonA.center = CGPointMake(faceCenterX + 12.0 * scale,
                                          faceCenterY + 20.0 * scale);
        self.buttonB.center = CGPointMake(faceCenterX - faceOffset,
                                          faceCenterY + faceOffset * 0.65);
        self.buttonX.center = CGPointMake(faceCenterX + faceOffset,
                                          faceCenterY - faceOffset * 0.55);
        self.buttonY.center = CGPointMake(faceCenterX - 8.0 * scale,
                                          faceCenterY - faceOffset);
    } else {
        // Xbox: Y top, X left, B right, A bottom.
        self.buttonY.center = CGPointMake(faceCenterX, faceCenterY - faceOffset);
        self.buttonX.center = CGPointMake(faceCenterX - faceOffset, faceCenterY);
        self.buttonB.center = CGPointMake(faceCenterX + faceOffset, faceCenterY);
        self.buttonA.center = CGPointMake(faceCenterX, faceCenterY + faceOffset);
    }

    CGFloat centerX = width/2;
    CGFloat systemWidth = 76.0 * scale;
    CGFloat systemHeight = 30.0 * scale;
    self.buttonBack.frame=CGRectMake(centerX-100*scale,height-bottom-systemHeight,systemWidth,systemHeight);
    self.buttonSelect.frame=CGRectMake(centerX-38*scale,height-bottom-systemHeight,systemWidth,systemHeight);
#if REMASKED_AZAHAR_TOUCH_HOST
    self.buttonPictograph.bounds = CGRectMake(0, 0, systemHeight, systemHeight);
    self.buttonPictograph.center = self.buttonSelect.center;
#endif
    self.buttonStart.frame=CGRectMake(centerX+24*scale,height-bottom-systemHeight,systemWidth,systemHeight);
    const CGFloat splitGap = 6.0 * scale;
    const CGFloat splitStartWidth = 68.0 * scale;
    const CGFloat splitActionsX = width-rightRail+(rightRail-splitStartWidth-2*(systemHeight+splitGap))/2;
    if (splitControls) {
        self.buttonBack.frame = CGRectMake((leftRail-systemWidth)/2,height-bottom-systemHeight,systemWidth,systemHeight);
        self.buttonStart.frame = CGRectMake(splitActionsX+systemHeight+splitGap,
                                             height-bottom-systemHeight,splitStartWidth,systemHeight);
    }
#if REMASKED_AZAHAR_TOUCH_HOST
    const bool cameraAsText = airPlayBottom && !splitControls;
    [self.buttonPictograph setImage:cameraAsText ? nil : [UIImage systemImageNamed:@"camera"]
                           forState:UIControlStateNormal];
    [self.buttonPictograph setTitle:cameraAsText ? @"Camera" : @"" forState:UIControlStateNormal];
    self.buttonPictograph.controlShape = cameraAsText ? ReMaskedButtonShapeRectangle : ReMaskedButtonShapeCircle;
    if (cameraAsText) {
        const CGFloat gap = 8*scale;
        const CGFloat buttonWidth = std::min(systemWidth, (width-left-right-2*gap)/3);
        const CGFloat rowWidth = 3*buttonWidth+2*gap;
        const CGFloat rowY = height-bottom-systemHeight;
        self.buttonBack.frame = CGRectMake(centerX-rowWidth/2,rowY,buttonWidth,systemHeight);
        self.buttonPictograph.frame = CGRectMake(centerX-buttonWidth/2,rowY,buttonWidth,systemHeight);
        self.buttonStart.frame = CGRectMake(centerX+buttonWidth/2+gap,rowY,buttonWidth,systemHeight);
    }
    if (splitControls) {
        self.buttonPictograph.frame = CGRectMake(splitActionsX,height-bottom-systemHeight,
                                                systemHeight,systemHeight);
    }
#endif

    CGFloat toggleSize = 30.0 * scale;
    self.visibilityButton.frame =
        CGRectMake(width - safe.right - toggleSize - 10.0 * scale,
                   height - safe.bottom - toggleSize - 10.0 * scale,
                   toggleSize, toggleSize);
    if (splitControls)
        self.visibilityButton.frame = CGRectMake(splitActionsX+systemHeight+splitStartWidth+2*splitGap,
                                                 height-bottom-systemHeight,toggleSize,toggleSize);
    self.visibilityButton.center=CGPointMake(self.visibilityButton.center.x+content.origin.x,self.visibilityButton.center.y+content.origin.y);

    [self.controlGroup sendSubviewToBack:self.rightStick];
    [self.controlGroup sendSubviewToBack:self.leftStick];
    [self.buttonX setTitle:@"" forState:UIControlStateNormal];
    self.buttonX.accessibilityLabel=@"Item X";
    self.buttonA.accessibilityLabel=@"Action";self.buttonB.accessibilityLabel=@"Sword";self.buttonY.accessibilityLabel=@"Item Y";
    self.buttonL.accessibilityLabel=@"Item I";self.buttonR.accessibilityLabel=@"Item II";self.buttonZL.accessibilityLabel=@"Target";self.buttonZR.accessibilityLabel=@"Shield";
    for(ReMaskedTouchButton* shoulder in @[self.buttonL,self.buttonR,self.buttonZL,self.buttonZR])
        [shoulder setTitle:sShoulderLabels&&!airPlayBottom?(shoulder==self.buttonL?@"L":shoulder==self.buttonR?@"R":shoulder==self.buttonZL?@"ZL":@"ZR"):@"" forState:UIControlStateNormal];

    for(ReMaskedTouchButton* button in @[self.buttonA,self.buttonB,self.buttonX,self.buttonY,self.buttonZR]){
        button.hudIcon=self.hudIcons && (button==self.buttonZR?self.manualJump==2:!(button==self.buttonX && self.manualJump==2));
        [button setNeedsLayout];
    }
    self.buttonMap.hidden=YES;
    self.buttonSelect.hidden=YES;
    self.buttonMap.frame=CGRectMake(CGRectGetMaxX(self.cCross.frame)+12*scale,
        CGRectGetMidY(self.cCross.frame)-18*scale,36*scale,36*scale);
    if (airPlayBottom) {
        self.leftStick.customPlaced = splitControls;
        self.rightStick.customPlaced = NO;
        self.rightStick.dragLook = sDragLook;
    } else {
        [self applyPlacements];
    }
    [self applyVisibility];
#if !REMASKED_AZAHAR_TOUCH_HOST
    ReMaskedIOSPublishAnchors(self);
#endif
}

- (void)cancelAllInputs {
    [self.leftStick cancelInput];
    [self.rightStick cancelInput];
    for (ReMaskedTouchButton* button in self.allInputButtons) {
        [button cancelInput];
    }
}

@end

static ReMaskedTouchOverlay* sTouchOverlay;
static unsigned TouchGeneration=1;
static BOOL sTouchControlsDesired;
static NSInteger sTouchFaceButtonLayout;
static BOOL sTouchLeftStickFloating = YES;
static std::atomic_int sTouchControlsMenuVisible(1);
static std::atomic_int sTouchDualDesired(0);
static float sTouchControlScale=1.f;

static UIWindow* ActiveWindow() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) {
            continue;
        }
        for (UIWindow* window in ((UIWindowScene*)scene).windows) {
            if (window.isKeyWindow) {
                return window;
            }
        }
    }
    return UIApplication.sharedApplication.windows.firstObject;
}

static void ApplyTouchControlsState() {
    UIWindow* window = ActiveWindow();
    if (window == nil) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                           ApplyTouchControlsState();
                       });
        return;
    }

    if (!sTouchControlsDesired) {
        [sTouchOverlay cancelAllInputs];
        [sTouchOverlay removeFromSuperview];
        sTouchOverlay = nil;
        return;
    }

    if (sTouchOverlay == nil) {
        sTouchOverlay = [[ReMaskedTouchOverlay alloc] initWithFrame:window.bounds];
        sTouchOverlay.dualLayout=sTouchDualDesired.load()!=0;[sTouchOverlay loadPlacements];
        sTouchOverlay.autoresizingMask =
            UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    }
    [sTouchOverlay setReMaskedFaceButtonLayout:sTouchFaceButtonLayout];
    sTouchOverlay.leftStick.floating = sTouchLeftStickFloating;
    sTouchOverlay.controlScale=sTouchControlScale;
    [sTouchOverlay setNeedsLayout];
    if (sTouchOverlay.superview != window) {
        [sTouchOverlay removeFromSuperview];
        sTouchOverlay.frame = window.bounds;
        [window addSubview:sTouchOverlay];
    }
    [sTouchOverlay setReMaskedMenuVisible:sTouchControlsMenuVisible.load()!=0];
    sTouchOverlay.hidden=sTouchControlsMenuVisible.load()!=0 && !sTouchOverlay.layoutEditor;
    [window bringSubviewToFront:sTouchOverlay];
}

int ReMaskedIOS_TouchControlsAvailable(void) {
    return 1;
}

void ReMaskedIOS_SetTouchControlsEnabled(int enabled) {
    OnTouchMain(^{
        sTouchControlsDesired = YES;
        ApplyTouchControlsState();
#if REMASKED_AZAHAR_TOUCH_HOST
        // Resuming gameplay or switching AirPlay layouts reapplies the same
        // preference. Keep the user's temporary visibility choice in that case.
        if (sTouchOverlay.controlsEnabled != (enabled != 0)) {
            [sTouchOverlay cancelAllInputs];
            sTouchOverlay.controlsEnabled = enabled != 0;
            sTouchOverlay.controlsHiddenByUser = enabled == 0;
        }
#else
        sTouchOverlay.controlsEnabled = enabled != 0;
        sTouchOverlay.controlsHiddenByUser = NO;
#endif
        [sTouchOverlay applyVisibility];
    });
}

void ReMaskedIOS_SetFaceButtonLayout(int layout) {
    OnTouchMain(^{
        sTouchFaceButtonLayout = std::clamp(layout, 0, 2);
        [sTouchOverlay setReMaskedFaceButtonLayout:sTouchFaceButtonLayout];
    });
}

void ReMaskedIOS_SetTouchLeftStickFloating(int floating) {
    OnTouchMain(^{
        sTouchLeftStickFloating = floating != 0;
        sTouchOverlay.leftStick.floating = sTouchLeftStickFloating;
    sTouchOverlay.controlScale=sTouchControlScale;
    });
}

void ReMaskedIOS_SetTouchControlsMenuVisible(int visible) {
    if(sTouchControlsMenuVisible.exchange(visible)==visible)return;
    OnTouchMain(^{
        ApplyTouchControlsState();
    });
}


void ReMaskedIOS_SetTouchOpacity(float opacity) {
    OnTouchMain(^{
        sArtworkOpacity=std::clamp(opacity,0.f,1.f);
        if(!sTouchOverlay)return;
        for(ReMaskedTouchButton* button in sTouchOverlay.allInputButtons) {
            [button setTitleColor:AndroidControlStroke() forState:UIControlStateNormal];
            [button setNeedsLayout];
        }
        for(ReMaskedTouchStick* stick in @[sTouchOverlay.leftStick,sTouchOverlay.rightStick]) {
            stick.base.layer.borderColor=AndroidControlStroke().CGColor;
            stick.knob.layer.borderColor=AndroidControlStroke().CGColor;
            stick.knob.backgroundColor=UIColor.clearColor;
        }
        [sTouchOverlay.cCross setNeedsDisplay];
    });
}

void ReMaskedIOS_SetTouchScale(float scale) {
    OnTouchMain(^{
        sTouchControlScale=std::clamp(scale,.5f,2.f);
        [sTouchOverlay cancelAllInputs];
        sTouchOverlay.controlScale=sTouchControlScale;
        [sTouchOverlay setNeedsLayout];
    });
}

void ReMaskedIOS_SetExtraShoulders(int enabled) {
    OnTouchMain(^{
        [sTouchOverlay cancelAllInputs];
        sTouchOverlay.extraShoulders=enabled!=0;
        [sTouchOverlay setNeedsLayout];
        [sTouchOverlay applyVisibility];
    });
}

void ReMaskedIOS_SetTouchHudIcons(int enabled) {
    OnTouchMain(^{
        if(!sTouchOverlay)return;
        sTouchOverlay.hudIcons=enabled!=0;
        [sTouchOverlay setNeedsLayout];
        sTouchOverlay.cCross.hudIcons=enabled!=0;
        [sTouchOverlay.cCross setNeedsDisplay];
        for(ReMaskedTouchButton* button in @[sTouchOverlay.buttonA,sTouchOverlay.buttonB,sTouchOverlay.buttonX,sTouchOverlay.buttonY]){
            button.hudIcon=enabled!=0;[button setNeedsLayout];
        }
    });
}

void ReMaskedIOS_SetManualJump(int mode) {
    OnTouchMain(^{
        if(!sTouchOverlay)return;
        [sTouchOverlay cancelAllInputs];
        sTouchOverlay.manualJump=std::clamp(mode,0,2);
        [sTouchOverlay setNeedsLayout];
        [sTouchOverlay applyVisibility];
    });
}

void ReMaskedIOS_EditTouchLayout(void) {
    OnTouchMain(^{
        sTouchControlsDesired=YES;ApplyTouchControlsState();
        [sTouchOverlay startLayoutEditing];
    });
}
void ReMaskedIOS_SetTouchDualLayout(int dual) {
    if(sTouchDualDesired.exchange(dual)==dual)return;
    OnTouchMain(^{
        if(!sTouchOverlay || sTouchOverlay.dualLayout==(dual!=0))return;
        if(sTouchOverlay.layoutEditor)[sTouchOverlay finishLayoutEditing:NO];
        sTouchOverlay.dualLayout=dual!=0;[sTouchOverlay loadPlacements];
        [sTouchOverlay cancelAllInputs];[sTouchOverlay setNeedsLayout];
    });
}

void ReMaskedIOS_SetTouchOptions(int labels,int drag) {
    OnTouchMain(^{
        [sTouchOverlay cancelAllInputs];sShoulderLabels=labels!=0;sDragLook=drag!=0;
        [sTouchOverlay setNeedsLayout];
    });
}
void ReMaskedIOS_SetControlSize(int control,float scale) {
    if(control<0||control>=15||!std::isfinite(scale))return;
    OnTouchMain(^{
        [sTouchOverlay cancelAllInputs];sIndividualSizes[control]=std::clamp(scale,.5f,2.f);
        [sTouchOverlay setNeedsLayout];
    });
}

#if REMASKED_AZAHAR_TOUCH_HOST
void ReMaskedAzaharSetTouchVisibility(unsigned hiddenControls, bool hideWithController) {
    OnTouchMain(^{
        if (sHiddenTouchControls == hiddenControls && sHideWithController == hideWithController) return;
        sHiddenTouchControls = hiddenControls;
        sHideWithController = hideWithController;
        [sTouchOverlay cancelAllInputs];
        [sTouchOverlay setNeedsLayout];
    });
}

void ReMaskedAzaharResetTouchLayout() {
    OnTouchMain(^{
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        for (NSString* layout in @[@"Single", @"Dual", @"Portrait"]) {
            NSString* key = [NSString stringWithFormat:@"ReMasked.TouchLayout.%@.v1", layout];
            [defaults removeObjectForKey:key];
            [defaults removeObjectForKey:[key stringByAppendingString:@".sizes"]];
        }
        [sTouchOverlay cancelAllInputs];
        [sTouchOverlay loadPlacements];
        [sTouchOverlay setNeedsLayout];
    });
}

static void ReMaskedIOSPublishAnchors(ReMaskedTouchOverlay* overlay) {
    if (!overlay.window || overlay.controlsHiddenByUser ||
        overlay.menuVisible || overlay.layoutEditor || overlay.portraitLayout) {
        ReMaskedAzaharPublishTouchHud(nullptr);
        return;
    }
    NSArray<UIView*>* controls=@[overlay.buttonA,overlay.buttonB,overlay.buttonX,overlay.buttonY,
        overlay.buttonL,overlay.buttonR,overlay.cButtons[0],overlay.cButtons[1],
        overlay.cButtons[2],overlay.cButtons[3]];
    CGRect frames[10]{};
    for (unsigned i=0;i<10;++i) {
        UIView* control=controls[i];
        if (!control.hidden) frames[i]=[control convertRect:control.bounds toView:overlay.window];
    }
    ReMaskedAzaharPublishTouchHud(frames);
}

void ReMaskedIOS_RefreshTouchLayout() {
    [sTouchOverlay setNeedsLayout];
    [sTouchOverlay layoutIfNeeded];
}
#else
// Same virtual-pad vocabulary and anchor ownership used by Android.
void ReMaskedIOSTouchButton(int button,bool pressed) {
    using namespace ThreeDsRecomp::Input;
    auto& pad=TouchOverlayPad();
    if(pressed&&sVibration){UIImpactFeedbackGenerator* feedback=[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];[feedback impactOccurred];}
    if(button==4||button==7){if(pressed)pad.RequestMenu();return;}
    int bit=-1;
    switch(button){case 0:bit=0;break;case 1:bit=1;break;case 2:bit=10;break;case 3:bit=11;break;
      case 6:bit=3;break;case 9:bit=14;break;case 10:bit=15;break;
      case 11:bit=6;break;case 12:bit=7;break;case 13:bit=5;break;case 14:bit=4;break;default:return;}
    DigitalControl control{};if(DigitalControlForHidBit(bit,&control))pad.SetControl(control,pressed);
}
void ReMaskedIOSTouchAxis(int axis,int value){
    using namespace ThreeDsRecomp::Input;
    static float axes[4]{};auto& pad=TouchOverlayPad();
    if(axis==4||axis==5){DigitalControl c{};DigitalControlForHidBit(axis==4?9:8,&c);pad.SetControl(c,value>0);return;}
    if(axis<0||axis>3)return;axes[axis]=std::clamp(value/32767.f,-1.f,1.f);
    if(axis<2)pad.SetCirclePad(axes[0],-axes[1]);else pad.SetCStick(axes[2],-axes[3]);
}
static void ReMaskedIOSPublishAnchors(ReMaskedTouchOverlay* overlay){
    using namespace ThreeDsRecomp::Input;
    auto& a=OverlayTouchAnchors();
    const bool enabled=overlay&&!overlay.hidden&&!overlay.menuVisible&&!overlay.controlsHiddenByUser&&overlay.controlsEnabled&&!overlay.layoutEditor&&overlay.hudIcons&&(!overlay.portraitLayout||sPortraitArtwork);
    const bool external=enabled&&overlay.portraitLayout&&sPortraitArtwork;
    if(Mm3dDekui::TouchHud::external_artwork.exchange(external)!=external)
        NSLog(@"REMASKED_PORTRAIT_ARTWORK active=%d fallback=%d",external,Mm3dDekui::TouchHud::external_failed.load());
    const float w=external?overlay.bounds.size.width:overlay.controlGroup.bounds.size.width;
    const float h=external?overlay.bounds.size.height:overlay.controlGroup.bounds.size.height;
    Mm3dDekui::TouchHud::external_canvas_width=h>0?240.f*w/h:240.f*16.f/9.f;
    Mm3dDekui::TouchHud::external_content_scale=h>0?390.f/h:1.f;
    if(!enabled||w<=0||h<=0){for(unsigned i=0;i<kTouchAnchorCount;++i)a.Clear(TouchAnchor(i));return;}
    NSArray<UIView*>* controls=@[overlay.buttonA,overlay.buttonB,overlay.buttonX,overlay.buttonY,overlay.buttonL,overlay.buttonR,overlay.cCross];
    for(unsigned i=0;i<7;++i){UIView* v=controls[i];CGRect r=[v convertRect:v.bounds toView:external?overlay:overlay.controlGroup];
        if(v.hidden)a.Clear(TouchAnchor(i));else a.Set(TouchAnchor(i),r.origin.x/w,r.origin.y/h,CGRectGetMaxX(r)/w,CGRectGetMaxY(r)/h);}
}
void ReMaskedIOSFrame(){
    using namespace ThreeDsRecomp::Input;
    ReMaskedGraphics::Publish();
    ReMaskedIOSNativeMenuFrame();
    static bool initialized=false;static uint32_t revision=0,edit=0,reset=0;static bool menuBefore=false;
    static unsigned generation=0;static std::string previous;
    if(generation!=TouchGeneration){generation=TouchGeneration;initialized=false;previous.clear();}
    NSUserDefaults* defaults=NSUserDefaults.standardUserDefaults;
    auto values=ReMaskedMenu::Json::object();
    NSData* data=[defaults dataForKey:@"ReMasked.TouchSettings.v1"];
    if(data)try{values=ReMaskedMenu::Json::parse(std::string((const char*)data.bytes,data.length));}catch(...){}
    if(!initialized){initialized=true;ReMaskedIOS_SetTouchControlsEnabled(values.value("visible",true));ReMaskedIOS_SetExtraShoulders(1);}
    auto req=OverlayTouchSettings().PendingRequests();
    if(req.Revision!=revision){revision=req.Revision;values["visible"]=req.Values.Visible;values["controlOpacity"]=req.Values.OpacityPercent;values["controlScale"]=req.Values.SizePercent-50;values["vibration"]=req.Values.Vibration;values["hideWithController"]=req.Values.HideWithController;}
    auto patch=ReMaskedMenu::TakeTouch();
    if(patch.contains("appIconSelection"))ReMaskedIOSApplyIcon(patch["appIconSelection"].get<int>());
    for(auto& [key,value]:patch.items())values[key]=value;
    values["dataDirectory"]=std::filesystem::current_path().string();
    values["lowerDisplayAvailable"]=ReMaskedDuo::GameDisplay.Available();
    values["dualScreen"]=ReMaskedAppleDuoRequested()?1:0;
    if(req.Revision||!patch.empty()){
        auto json=values.dump();[defaults setObject:[NSData dataWithBytes:json.data() length:json.size()] forKey:@"ReMasked.TouchSettings.v1"];
    }
    ReMaskedMenu::ReportTouch(values);
    ReMaskedSettings::UserMenuScale=values.value("menuScale",100)/100.f;
    sVibration=values.value("vibration",true);
    const bool menu=ReMaskedMenu::Visible.load();
    menuBefore=menu;ReMaskedIOS_SetTouchControlsMenuVisible(menu);
    ReMaskedIOS_SetTouchDualLayout(ReMaskedDual::Active.load());
    auto serialized=values.dump();if(serialized!=previous){previous=serialized;
        ReMaskedIOS_SetTouchControlsEnabled(values.value("visible",true));
        ReMaskedIOS_SetTouchOpacity(values.value("controlOpacity",28)/100.f);
        ReMaskedIOS_SetTouchScale((values.value("controlScale",50)+50)/100.f);
        ReMaskedIOS_SetFaceButtonLayout(values.value("faceLayout",0));
        ReMaskedIOS_SetTouchLeftStickFloating(values.value("floating",true));
        ReMaskedIOS_SetTouchOptions(values.value("shoulderLabels",false),values.value("dragLook",false));
        ReMaskedIOS_SetTouchHudIcons(values.value("itemIcons",true));
        constexpr int ids[]={700,701,702,703,773,774,707,708,704,705,780,713,718};
        constexpr int indexes[]={0,1,2,3,4,5,6,7,8,9,10,11,12};
        for(unsigned i=0;i<13;++i)ReMaskedIOS_SetControlSize(indexes[i],(values.value("controlScale-"+std::to_string(ids[i]),50)+50)/100.f);
        Oot3dNativeGame::Mm3dSingleScreen::SetTouchIconOpacity(values.value("itemIconOpacity",100)/100.f);
        std::array<float,6> scales{};constexpr int hudids[]{700,702,703,701,773,774};
        for(unsigned i=0;i<6;++i)scales[i]=(values.value("controlScale-"+std::to_string(hudids[i]),50)+50)/100.f;
        Oot3dNativeGame::Mm3dSingleScreen::SetTouchIconScales(scales);
    }
    if(!values.value("portraitTouchArtwork",false))Mm3dDekui::TouchHud::external_failed=false;
    sPortraitArtwork=values.value("portraitTouchArtwork",false)&&!Mm3dDekui::TouchHud::external_failed.load();
    OverlayTouchSettings().Report({values.value("visible",true),values.value("controlOpacity",28),values.value("controlScale",50)+50,values.value("vibration",true),values.value("hideWithController",true)},revision);
    if(req.EditLayoutSerial!=edit){edit=req.EditLayoutSerial;ReMaskedIOS_EditTouchLayout();}
    if(req.ResetLayoutSerial!=reset){reset=req.ResetLayoutSerial;sTouchOverlay.placements=[NSMutableDictionary dictionary];sTouchOverlay.sizes=[NSMutableDictionary dictionary];[defaults removeObjectForKey:[sTouchOverlay placementPreference]];[defaults removeObjectForKey:[[sTouchOverlay placementPreference] stringByAppendingString:@".sizes"]];[sTouchOverlay setNeedsLayout];}
    if(sTouchOverlay){
        sTouchOverlay.extraShoulders=YES;
        static int previousAspect=-1;
        const auto portrait=ReMaskedIOS::PhonePortrait.Pixels(sTouchOverlay.bounds.size.width,sTouchOverlay.bounds.size.height);
        static CGSize lastPortraitBounds=CGSizeZero;
        if(sTouchOverlay.portraitLayout!=portrait.Active() || !CGSizeEqualToSize(lastPortraitBounds,sTouchOverlay.bounds.size)){
            lastPortraitBounds=sTouchOverlay.bounds.size;[sTouchOverlay setNeedsLayout];
            NSLog(@"REMASKED_PORTRAIT_CONTROLS bounds=%@ active=%d",NSStringFromCGSize(lastPortraitBounds),portrait.Active());
        }
        const int aspect=int(Fast::Extension::GraphicsSettingsRuntime::Instance().Snapshot().Aspect);
        if(aspect!=previousAspect){previousAspect=aspect;[sTouchOverlay setNeedsLayout];}
        static bool wasController=false;
        bool controller=values.value("hideWithController",true)&&GCController.controllers.count>0;
        if(controller!=wasController){wasController=controller;[sTouchOverlay cancelAllInputs];sTouchOverlay.controlsHiddenByUser=controller;}
        [sTouchOverlay applyVisibility];
        NSArray<UIView*>* ordered=@[sTouchOverlay.buttonA,sTouchOverlay.buttonB,sTouchOverlay.buttonX,sTouchOverlay.buttonY,sTouchOverlay.buttonL,sTouchOverlay.buttonR,sTouchOverlay.buttonZL,sTouchOverlay.buttonZR,sTouchOverlay.buttonStart,sTouchOverlay.buttonSelect,sTouchOverlay.cCross,sTouchOverlay.leftStick,sTouchOverlay.rightStick];
        for(unsigned i=0;i<13;++i)if(!values.value("buttonToggle"+std::to_string(i),true)){ordered[i].hidden=YES;if(i==10)for(UIView* v in sTouchOverlay.cButtons)v.hidden=YES;}
        if(sTouchOverlay.layoutEditor||(!sTouchOverlay.hidden&&!sTouchOverlay.menuVisible))[sTouchOverlay.superview bringSubviewToFront:sTouchOverlay];
        [sTouchOverlay layoutIfNeeded];ReMaskedIOSPublishAnchors(sTouchOverlay);
        // Equipped artwork/counters and A text come from the game's HD HUD.
        const bool draws=OverlayTouchAnchors().TitleDrawsControls()&&sTouchOverlay.hudIcons&&!sTouchOverlay.portraitLayout;
        for(ReMaskedTouchButton* b in @[sTouchOverlay.buttonA,sTouchOverlay.buttonB,sTouchOverlay.buttonX,sTouchOverlay.buttonY,sTouchOverlay.buttonL,sTouchOverlay.buttonR])b.artwork.hidden=draws;
        sTouchOverlay.cCross.alpha=draws?0:1;
    }
}

#endif

void ReMaskedIOS_StopTouchControls(void){
 ++TouchGeneration;sTouchControlsDesired=NO;sTouchControlsMenuVisible=1;
 [sTouchOverlay cancelAllInputs];[sTouchOverlay removeFromSuperview];sTouchOverlay=nil;
#if REMASKED_AZAHAR_TOUCH_HOST
 ReMaskedAzaharPublishTouchHud(nullptr);
 ReMaskedAzaharReleaseTouch();
#else
 Mm3dDekui::TouchHud::external_artwork=false;
 auto& anchors=ThreeDsRecomp::Input::OverlayTouchAnchors();
 for(unsigned i=0;i<ThreeDsRecomp::Input::kTouchAnchorCount;++i)anchors.Clear(ThreeDsRecomp::Input::TouchAnchor(i));
#endif
}
