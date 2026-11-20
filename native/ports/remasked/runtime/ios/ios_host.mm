#include "../model_stature.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// iOS owns file access and the launcher; settings values belong to the same
// document and persistence owners as Android. No game renderer before Start.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <CommonCrypto/CommonDigest.h>
#include "ios_host.h"
#include "aim_modes.h"
#include "drive_data.h"
#include "prepared_game.h"
#include "oot3d_native_a32_process_image.h"
#include "save_sync.h"
#include "touch_controls.h"
#include "fast/extension/remasked_dual_screen.h"
#include "apple_duo.h"
#import "duo_layout_observer.h"
#include "fast/extension/remasked_duo_layout.h"
#include "android_menu.h"
#include "android_graphics_support.h"
#include "controller_layout.h"
#include "oot3d_menu_font.h"
#include "three_ds_touch_overlay_settings.h"
#include "three_ds_virtual_pad.h"
#include "ship/audio/AudioOutputPolicy.h"
#include "fast/extension/graphics_settings_runtime.h"
#include "fast/extension/graphics_settings_persistence.h"
#include <memory>
#include <fstream>
using namespace Oot3dNativeGame;
using Json=nlohmann::json;
namespace {
void OnMain(dispatch_block_t work){if(NSThread.isMainThread)work();else dispatch_async(dispatch_get_main_queue(),work);}
NSString* Text(const std::string& s){return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]?:@"";}
UIColor* Purple(){return [UIColor colorWithRed:.725 green:.49 blue:.949 alpha:1];}
UIColor* Background(){return [UIColor colorWithRed:.031 green:.027 blue:.051 alpha:1];}
struct Store final:Fast::Extension::GraphicsSettingsPersistencePort,Ship::AudioOutputPersistencePort{
 std::filesystem::path Path;explicit Store(std::filesystem::path p):Path(std::move(p)){}
 bool LoadRoot(Json& root)override{try{root=ReMaskedMenu::ReadFile(Path);return true;}catch(...){return false;}}
 bool Put(const char* key,const Json& value){try{Json root=Json::object();LoadRoot(root);root[key]=value;ReMaskedMenu::AtomicWrite(Path,root);return true;}catch(...){return false;}}
 bool StoreGraphics(const Json& j)override{return Put("Graphics",j);}bool StoreGraphicsPresets(const Json& j)override{return Put("GraphicsPresets",j);}
 bool LoadAudio(Json& j)override{Json root;if(!LoadRoot(root))return false;j=root.value("Audio",Json{});return true;}
 bool StoreAudio(const Json& j,std::string* e)override{if(Put("Audio",j))return true;if(e)*e="Audio could not be saved";return false;}
};
std::unique_ptr<InGameMenu> Startup;
InGameMenu* LiveMenu=nullptr;
bool PresentingSettings=false;
InGameMenu* SettingsOwner(){return LiveMenu?LiveMenu:Startup.get();}
Menu::Model& SettingsModel(){return SettingsOwner()->Model();}
bool StartRequested=false,GameRunning=false,LauncherShown=false;
bool DriveChecked=false;
void RebuildStartup();
void (*ResumeMain)()=nullptr;
UIWindow* LauncherWindow=nil;
NSTimer* LauncherTimer=nil;
void FinishStartupLaunch();
std::filesystem::path Root(){return std::filesystem::current_path();}
namespace fs = std::filesystem;

fs::path PreparedContent() {
    return Root() / ReMaskedIOS::PreparedGameDirectory;
}

fs::path BundledContent() {
    return fs::path(NSBundle.mainBundle.resourcePath.UTF8String) / ReMaskedIOS::PreparedGameDirectory;
}

bool HasGameContent() {
    const auto manifest = ReMaskedIOS::PreparedGameManifest;
    return fs::is_regular_file(PreparedContent() / manifest) ||
           fs::is_regular_file(BundledContent() / manifest);
}

void ValidatePreparedGame(const fs::path& directory) {
    const auto manifestPath = directory / ReMaskedIOS::PreparedGameManifest;
    if (fs::file_size(manifestPath) > 1024 * 1024 ||
        fs::file_size(directory / "code.bin") > 32ULL * 1024 * 1024 ||
        fs::file_size(directory / "exheader.bin") != 2048) {
        throw std::runtime_error("The prepared folder contains an invalid file size.");
    }
    const auto manifest = ReMaskedMenu::ReadFile(manifestPath);
    const auto& source = manifest.at("source");
    constexpr auto expectedCode = "6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d";
    if (source.at("code_bin_path") != "code.bin" ||
        source.at("exheader_path") != "exheader.bin" ||
        source.at("romfs_image_path") != "romfs.bin" ||
        source.at("code_bin_sha256") != expectedCode) {
        throw std::runtime_error("The prepared folder does not match this runtime.");
    }
    NSData* code = [NSData dataWithContentsOfFile:Text((directory / "code.bin").string())];
    if (!code || code.length > 32 * 1024 * 1024) {
        throw std::runtime_error("The prepared code image is missing or invalid.");
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(code.bytes, static_cast<CC_LONG>(code.length), digest);
    NSMutableString* hash = [NSMutableString string];
    for (auto byte : digest) [hash appendFormat:@"%02x", byte];
    if (![hash isEqualToString:Text(expectedCode)]) {
        throw std::runtime_error("The prepared code image does not match this runtime.");
    }
    std::string error;
    if (!LoadNativeA32ProcessImageManifest(manifestPath, &error)) {
        throw std::runtime_error("The prepared game manifest or file layout is invalid.");
    }
}

void ImportPreparedGame(const fs::path& source, const fs::path& destination) {
    if (fs::exists(destination)) {
        throw std::runtime_error("Game files are already installed locally.");
    }
    fs::create_directories(destination.parent_path());
    const auto staging = destination.parent_path() / NSUUID.UUID.UUIDString.UTF8String;
    fs::create_directory(staging);
    try {
        const std::pair<const char*, uint64_t> limits[] = {
            {"code.bin", 32ULL * 1024 * 1024},
            {"exheader.bin", 2048},
            {"romfs.bin", 4ULL * 1024 * 1024 * 1024},
            {ReMaskedIOS::PreparedGameManifest, 1024 * 1024}
        };
        for (const auto& [name, maxSize] : limits) {
            const auto file = source / name;
            if (!fs::is_regular_file(fs::symlink_status(file)) ||
                fs::file_size(file) == 0 || fs::file_size(file) > maxSize) {
                throw std::runtime_error(std::string("Missing or invalid prepared file: ") + name);
            }
            fs::copy_file(file, staging / name);
        }
        ValidatePreparedGame(staging);
        fs::rename(staging, destination);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
}

void SaveTouch(){
 auto patch=ReMaskedMenu::TakeTouch();if(patch.contains("appIconSelection"))ReMaskedIOSApplyIcon(patch["appIconSelection"].get<int>());
 Json values=Json::object();NSData* old=[NSUserDefaults.standardUserDefaults dataForKey:@"ReMasked.TouchSettings.v1"];if(old)try{values=Json::parse(std::string((const char*)old.bytes,old.length));}catch(...){}
 auto request=ThreeDsRecomp::Input::OverlayTouchSettings().PendingRequests();
 if(request.Revision){auto v=request.Values;values["visible"]=v.Visible;values["controlOpacity"]=v.OpacityPercent;values["controlScale"]=v.SizePercent-50;values["vibration"]=v.Vibration;values["hideWithController"]=v.HideWithController;}
 for(auto& [k,v]:patch.items())values[k]=v;
 auto text=values.dump();[NSUserDefaults.standardUserDefaults setObject:[NSData dataWithBytes:text.data() length:text.size()] forKey:@"ReMasked.TouchSettings.v1"];ReMaskedMenu::ReportTouch(values);
}

}
@interface ReMaskedIOSController : UIViewController<UITableViewDataSource,UITableViewDelegate,UIDocumentPickerDelegate>
@property(nonatomic,strong) UITableView* categories;
@property(nonatomic,strong) UITableView* rows;
@property(nonatomic,strong) UILabel* status;
@property(nonatomic,strong) UIStackView* launcher;
@property(nonatomic,strong) UIImageView* masks;
@property(nonatomic,strong) UILabel* subtitle;
@property(nonatomic,strong) ReMaskedDuoLayoutObserver* layoutObserver;
@property(nonatomic) BOOL settings;
@property(nonatomic) NSInteger page;
@property(nonatomic,copy) NSString* pickerPurpose;
- (void)reload;
- (void)showModelModal;
- (void)route:(NSString*)route;
- (void)syncDrive:(NSURL*)selected resolution:(ReMaskedDrive::Resolution)resolution;
@end
static __weak ReMaskedIOSController* ActiveController;
static __weak UIViewController* GamePresenter;
static UIViewController* Presenter(){if(ActiveController)return ActiveController;for(UIScene* s in UIApplication.sharedApplication.connectedScenes)if([s isKindOfClass:UIWindowScene.class])for(UIWindow* w in ((UIWindowScene*)s).windows)if(w.isKeyWindow){UIViewController* v=w.rootViewController;while(v.presentedViewController)v=v.presentedViewController;return v;}return nil;}
static void Notice(NSString* title,NSString* message){UIAlertController* a=[UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];[Presenter() presentViewController:a animated:YES completion:nil];}
@implementation ReMaskedIOSController
- (void)viewDidLoad{
 [super viewDidLoad];self.view.backgroundColor=Background();self.view.tintColor=Purple();self.overrideUserInterfaceStyle=UIUserInterfaceStyleDark;
 UIButton* back=[UIButton buttonWithType:UIButtonTypeSystem];[back setTitle:@"Back" forState:UIControlStateNormal];[back addTarget:self action:@selector(back) forControlEvents:UIControlEventTouchUpInside];back.tag=110;[self.view addSubview:back];
 UILabel* title=[UILabel new];title.text=@"Majora ReMasked";title.textColor=Purple();title.font=[UIFont boldSystemFontOfSize:24];title.tag=111;[self.view addSubview:title];
 self.launcher=[[UIStackView alloc] init];self.launcher.axis=UILayoutConstraintAxisVertical;self.launcher.spacing=18;self.launcher.alignment=UIStackViewAlignmentFill;
 self.masks=[[UIImageView alloc] initWithImage:[UIImage imageNamed:@"ReMaskedMasks.png"]];self.masks.contentMode=UIViewContentModeScaleAspectFit;[self.view addSubview:self.masks];
 self.subtitle=[UILabel new];self.subtitle.text=@"A DekUI Fork";self.subtitle.textColor=Purple();self.subtitle.textAlignment=NSTextAlignmentRight;self.subtitle.font=[UIFont italicSystemFontOfSize:16];[self.view addSubview:self.subtitle];
 for(NSString* label in @[@"Start game",@"Settings",@"About"]){UIButton* b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:label forState:UIControlStateNormal];b.titleLabel.font=[UIFont fontWithName:@"FiraSans-Bold" size:20];b.contentHorizontalAlignment=UIControlContentHorizontalAlignmentRight;b.backgroundColor=UIColor.clearColor;[b.heightAnchor constraintGreaterThanOrEqualToConstant:50].active=YES;[b addTarget:self action:@selector(launchAction:) forControlEvents:UIControlEventTouchUpInside];[self.launcher addArrangedSubview:b];}[self.view addSubview:self.launcher];
 self.categories=[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];self.rows=[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
 for(UITableView* t in @[self.categories,self.rows]){t.backgroundColor=Background();t.dataSource=self;t.delegate=self;t.separatorColor=[UIColor colorWithRed:.25 green:.17 blue:.33 alpha:1];t.rowHeight=UITableViewAutomaticDimension;t.estimatedRowHeight=72;[self.view addSubview:t];}
 self.status=[UILabel new];self.status.textColor=Purple();self.status.numberOfLines=2;self.status.font=[UIFont systemFontOfSize:13];[self.view addSubview:self.status];[self reload];
}
- (void)viewDidLayoutSubviews{[super viewDidLayoutSubviews];UIEdgeInsets safe=self.view.safeAreaInsets;CGFloat w=self.view.bounds.size.width-safe.left-safe.right,h=self.view.bounds.size.height-safe.top-safe.bottom,x=safe.left,y=safe.top;
 [self.view viewWithTag:111].frame=CGRectMake(x+20,y+8,MAX(0,w-100),40);[self.view viewWithTag:110].frame=CGRectMake(x+w-80,y+8,70,40);
 CGFloat nav=MIN(180,MAX(105,w*.26));self.categories.frame=CGRectMake(x+8,y+60,nav,h-105);self.rows.frame=CGRectMake(x+nav+16,y+60,MAX(0,w-nav-24),h-105);self.status.frame=CGRectMake(x+16,y+h-42,w-32,40);
  if(self.settings){
  [self.view viewWithTag:111].frame=CGRectMake(x+20,y+8,MAX(0,w-100),40);
  ((UILabel*)[self.view viewWithTag:111]).textAlignment=NSTextAlignmentLeft;
 }else{
  self.masks.frame=CGRectMake(x+w*.02,y+h*.16,w*.56,h*.78);
  [self.view viewWithTag:111].frame=CGRectMake(x+w*.31,y+12,w*.64,50);
  ((UILabel*)[self.view viewWithTag:111]).textAlignment=NSTextAlignmentRight;
  self.subtitle.frame=CGRectMake(x+w*.31,y+64,w*.64,24);
 }
 self.launcher.frame=CGRectMake(x+w*.36,y+MAX(110,h-210),w*.59,180);
}
- (void)showModelModal{
 if(!SettingsOwner()||self.presentedViewController||!SettingsModel().CurrentModal())return;
 auto modal=*SettingsModel().CurrentModal();UIAlertController* alert=[UIAlertController alertControllerWithTitle:Text(modal.Title) message:Text([&](){std::string text;for(auto& line:modal.Lines){if(!text.empty())text+="\n";text+=line;}return text;}()) preferredStyle:UIAlertControllerStyleAlert];
 [alert addAction:[UIAlertAction actionWithTitle:Text(modal.ConfirmLabel) style:UIAlertActionStyleDefault handler:^(UIAlertAction*){SettingsModel().SetModal(std::nullopt);if(modal.OnConfirm)modal.OnConfirm();[self reload];}]];
 if(!modal.CancelLabel.empty())[alert addAction:[UIAlertAction actionWithTitle:Text(modal.CancelLabel) style:UIAlertActionStyleCancel handler:^(UIAlertAction*){SettingsModel().SetModal(std::nullopt);if(modal.OnCancel)modal.OnCancel();[self reload];}]];
 [self presentViewController:alert animated:YES completion:nil];
}
- (void)reload{if(SettingsOwner())SettingsOwner()->Refresh();self.launcher.hidden=self.settings;self.masks.hidden=self.settings;self.subtitle.hidden=self.settings;[self.view setNeedsLayout];self.categories.hidden=!self.settings;self.rows.hidden=!self.settings;[self.view viewWithTag:110].hidden=!self.settings;[self.categories reloadData];[self.rows reloadData];[self showModelModal];}
- (void)back{if(LiveMenu){SettingsModel().Close();[self dismissViewControllerAnimated:YES completion:^{PresentingSettings=false;LiveMenu=nullptr;ActiveController=nil;}];}else{self.settings=NO;[self reload];}}
- (void)launchAction:(UIButton*)button{NSString* name=button.currentTitle;if([name isEqual:@"Start game"]){if(!HasGameContent()){[self route:@"remasked-prepare-game"];return;}SaveTouch();StartRequested=true;self.launcher.userInteractionEnabled=NO;self.status.text=@"Starting game…";NSLog(@"REMASKED_IOS_START requested by user");
 try{FinishStartupLaunch();if(ResumeMain)ResumeMain();}catch(const std::exception& error){StartRequested=false;self.launcher.userInteractionEnabled=YES;Notice(@"Could not start game",Text(error.what()));}}else if([name isEqual:@"Settings"]){self.settings=YES;[self reload];}else Notice(@"Majora ReMasked",@"A DekUI Fork\n\nSettings are available here and during play. Start opens the Items menu; Select opens app settings.");}
- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section{if(!SettingsOwner())return 0;auto& doc=SettingsModel().Doc();return table==self.categories?doc.Tabs.size():doc.Tabs.at(self.page).Rows.size();}
- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)index{
 UITableViewCell* c=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];c.backgroundColor=Background();c.textLabel.textColor=UIColor.whiteColor;c.detailTextLabel.textColor=[UIColor colorWithWhite:.65 alpha:1];c.textLabel.numberOfLines=0;c.detailTextLabel.numberOfLines=0;c.tintColor=Purple();
 auto& doc=SettingsModel().Doc();if(table==self.categories){c.textLabel.text=Text(doc.Tabs.at(index.row).Title);c.textLabel.font=[UIFont systemFontOfSize:15 weight:index.row==self.page?UIFontWeightBold:UIFontWeightRegular];if(index.row==self.page){c.backgroundColor=[UIColor colorWithRed:.2 green:.10 blue:.30 alpha:1];c.textLabel.textColor=Purple();}return c;}
 auto& r=doc.Tabs.at(self.page).Rows.at(index.row);c.textLabel.text=Text(r.Key);float size=1;{auto& t=ReMaskedMenu::Touch();std::lock_guard lock(t.Mutex);size=std::clamp(t.Values.value("menuScale",100),50,125)/100.f;}
 c.textLabel.font=[UIFont fontWithName:r.Kind==Menu::RowKind::Heading?@"FiraSans-Bold":@"FiraSans-Regular" size:16*size];c.detailTextLabel.font=[UIFont fontWithName:@"FiraSans-Regular" size:13*size];
 if(r.Kind==Menu::RowKind::Heading)c.textLabel.textColor=Purple();else if(r.Kind==Menu::RowKind::Info)c.detailTextLabel.text=Text(r.DynamicText?r.DynamicText():r.Text);else{c.detailTextLabel.text=Text(r.Enabled?r.Help:r.UnavailableReason);if(r.Notice){auto notice=r.Notice();if(!notice.empty())c.detailTextLabel.text=[NSString stringWithFormat:@"%@\n%@",c.detailTextLabel.text?:@"",Text(notice)];}if(!r.Enabled)c.textLabel.textColor=UIColor.grayColor;
 if(r.Kind==Menu::RowKind::Toggle){UISwitch* v=[UISwitch new];v.on=r.Value!=0;v.enabled=r.Enabled;v.onTintColor=Purple();v.tag=index.row;[v addTarget:self action:@selector(toggle:) forControlEvents:UIControlEventValueChanged];c.accessoryView=v;}
 else if(r.Kind==Menu::RowKind::Choice&&!r.ChoiceImages.empty()){
 UIStackView* stack=[UIStackView new];stack.axis=UILayoutConstraintAxisHorizontal;stack.spacing=6;CGFloat side=MAX(28,MIN(52,(self.rows.bounds.size.width-20)/5-6));stack.frame=CGRectMake(0,0,(side+6)*5,side+8);
 for(int i=0;i<5;++i){UIButton* b=[UIButton buttonWithType:UIButtonTypeCustom];[b setImage:[UIImage imageNamed:i==0?@"ReMaskedLauncher.png":[NSString stringWithFormat:@"ReMaskedIcon%d.png",i]] forState:UIControlStateNormal];b.tag=index.row*10+i;b.accessibilityLabel=Text(r.Choices[i]);b.layer.cornerRadius=8;b.clipsToBounds=YES;b.layer.borderWidth=r.Value==i?3:0;b.layer.borderColor=Purple().CGColor;[b.widthAnchor constraintEqualToConstant:side].active=YES;[b addTarget:self action:@selector(icon:) forControlEvents:UIControlEventTouchUpInside];[stack addArrangedSubview:b];}c.accessoryView=stack;
 }
 else if(r.Kind==Menu::RowKind::Choice){c.detailTextLabel.text=[NSString stringWithFormat:@"%@\n%@",Text(r.ValueText()),c.detailTextLabel.text?:@""];c.accessoryType=UITableViewCellAccessoryDisclosureIndicator;}
 else if(r.Kind==Menu::RowKind::Number){UIStackView* stack=[UIStackView new];stack.axis=UILayoutConstraintAxisVertical;stack.frame=CGRectMake(0,0,MIN(180,self.rows.bounds.size.width*.48),65);UILabel* text=[UILabel new];text.text=Text(r.ValueText());text.textColor=Purple();text.font=[UIFont systemFontOfSize:13];UISlider* slider=[UISlider new];slider.minimumValue=r.Minimum;slider.maximumValue=r.Maximum;slider.value=r.Value;slider.enabled=r.Enabled;slider.tag=index.row;slider.minimumTrackTintColor=Purple();[slider addTarget:self action:@selector(slide:) forControlEvents:UIControlEventTouchUpInside|UIControlEventTouchUpOutside];[stack addArrangedSubview:text];[stack addArrangedSubview:slider];c.accessoryView=stack;}}
 return c;
}
- (void)write:(NSInteger)row value:(int)value{auto& r=SettingsModel().Doc().Tabs.at(self.page).Rows.at(row);if(!r.Enabled||!r.Write)return;auto result=r.Write(value);SaveTouch();SettingsOwner()->Refresh();self.status.text=Text(result.Message.empty()?(result.Error?"Could not save":"Saved"):result.Message);[self.rows reloadData];}
- (void)icon:(UIButton*)sender{[self write:sender.tag/10 value:int(sender.tag%10)];}
- (void)toggle:(UISwitch*)sender{[self write:sender.tag value:sender.on];}
- (void)slide:(UISlider*)sender{auto& r=SettingsModel().Doc().Tabs.at(self.page).Rows.at(sender.tag);int step=MAX(1,r.Step);[self write:sender.tag value:std::clamp(r.Minimum+int(std::lround((sender.value-r.Minimum)/step))*step,r.Minimum,r.Maximum)];}
- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)index{if(table==self.categories){self.page=index.row;SettingsModel().SelectTab(index.row);[self reload];return;}auto& r=SettingsModel().Doc().Tabs.at(self.page).Rows.at(index.row);if(!r.Enabled)return;
 if(r.Kind==Menu::RowKind::Action&&r.Activate){auto result=r.Activate();SaveTouch();self.status.text=Text(result.Message);[self reload];}
 if(r.Kind==Menu::RowKind::Choice&&r.ChoiceImages.empty()){UIAlertController* a=[UIAlertController alertControllerWithTitle:Text(r.Key) message:nil preferredStyle:UIAlertControllerStyleActionSheet];for(int i=0;i<int(r.Choices.size());++i){if(r.ChoiceAvailable&&!r.ChoiceAvailable(i))continue;[a addAction:[UIAlertAction actionWithTitle:Text(r.Choices[i]) style:UIAlertActionStyleDefault handler:^(UIAlertAction*){[self write:index.row value:i];}]];}[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];a.popoverPresentationController.sourceView=[table cellForRowAtIndexPath:index];a.popoverPresentationController.sourceRect=[table cellForRowAtIndexPath:index].bounds;[self presentViewController:a animated:YES completion:nil];}
}
- (void)syncDrive:(NSURL*)selected resolution:(ReMaskedDrive::Resolution)resolution{
 if(GameRunning){Notice(@"Data folder",@"Return to the startup screen before syncing data. Gameplay always uses local working files.");return;}
 if(ReMaskedSaveSyncBusy()){Notice(@"Data folder",@"Save sync is finishing. Please try again shortly.");return;}
 DriveChecked=true;SaveTouch();self.view.userInteractionEnabled=NO;self.status.text=@"Syncing data folder…";
 ReMaskedDrive::Sync([NSURL fileURLWithPath:Text(Root().string()) isDirectory:YES],selected,resolution,^(ReMaskedDrive::Result result){
  self.view.userInteractionEnabled=YES;self.status.text=result.message;
  if(result.conflict){
   UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Data folder conflict" message:@"Some files changed in both locations. Choose which copy to keep for those files. Other files merge automatically. Previous replaced files are preserved locally until sync completes." preferredStyle:UIAlertControllerStyleAlert];
   [alert addAction:[UIAlertAction actionWithTitle:@"Use local copies" style:UIAlertActionStyleDefault handler:^(UIAlertAction*){[self syncDrive:selected resolution:ReMaskedDrive::Resolution::Local];}]];
   [alert addAction:[UIAlertAction actionWithTitle:@"Use Drive copies" style:UIAlertActionStyleDefault handler:^(UIAlertAction*){[self syncDrive:selected resolution:ReMaskedDrive::Resolution::Drive];}]];
   [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];[self presentViewController:alert animated:YES completion:nil];
  }else if(result.ok){
   // Settings owners cache their files. Recreate them after importing changes,
   // before the guest is constructed, while retaining the current Data page.
   try{RebuildStartup();}catch(const std::exception& error){Notice(@"Data synced",[@"Reopen ReMasked to load the updated settings. " stringByAppendingString:Text(error.what())]);}
  }else Notice(@"Data sync",result.message);
 });
}
- (void)route:(NSString*)route{
 if ([route isEqual:@"remasked-prepare-game"] && !GameRunning) {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Import prepared game folder"
        message:@"Choose your prepared folder containing code.bin, exheader.bin, romfs.bin and process-manifest.json. ReMasked copies the files into local storage."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Choose folder" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        self.pickerPurpose = @"prepared-game";
        UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
        picker.delegate = self;
        [self presentViewController:picker animated:YES completion:nil];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
    return;
 }

 if([route isEqual:@"remasked-data-folder"]){if(GameRunning){Notice(@"Data folder",@"Choose a data folder from startup Settings after closing the game.");return;}self.pickerPurpose=@"data";UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];picker.delegate=self;[self presentViewController:picker animated:YES completion:nil];return;}
 if([route isEqual:@"remasked-data-sync"]){[self syncDrive:nil resolution:ReMaskedDrive::Resolution::Ask];return;}
 if([route isEqual:@"remasked-data-default"]){if(GameRunning)return;ReMaskedDrive::Disconnect();[self reload];self.status.text=ReMaskedDrive::Status();return;}

 if([route isEqual:@"remasked-pak"]){
  if(GameRunning){Notice(@"PAK Model Loader",@"Import prepared models from the startup screen after closing the game. This protects any model currently in use.");return;}
  UIAlertController* a=[UIAlertController alertControllerWithTitle:@"PAK Model Loader" message:@"Prepare the PAK on Android or desktop first, then copy its prepared model folder to Files. Choose the folder containing romfs/actors/zelda2_link_child_new.gar.lzs. Raw .pak files cannot be converted on iOS. Your existing model is retained until import succeeds." preferredStyle:UIAlertControllerStyleAlert];
  [a addAction:[UIAlertAction actionWithTitle:@"Choose prepared model folder" style:UIAlertActionStyleDefault handler:^(UIAlertAction*){self.pickerPurpose=@"model";UIDocumentPickerViewController* p=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];p.directoryURL=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;p.delegate=self;[self presentViewController:p animated:YES completion:nil];}]];// Prepared folders preloaded through Files/Finder are directly selectable.
  NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
  NSArray<NSURL*>* folders=[NSFileManager.defaultManager contentsOfDirectoryAtURL:documents includingPropertiesForKeys:nil options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
  for(NSURL* folder in folders){NSURL* actor=[folder URLByAppendingPathComponent:@"romfs/actors/zelda2_link_child_new.gar.lzs"];
   if(![NSFileManager.defaultManager fileExistsAtPath:actor.path])continue;
   [a addAction:[UIAlertAction actionWithTitle:[@"Import " stringByAppendingString:folder.lastPathComponent] style:UIAlertActionStyleDefault handler:^(UIAlertAction*){self.pickerPurpose=@"model";[self documentPicker:nil didPickDocumentsAtURLs:@[folder]];}]];
  }
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];[self presentViewController:a animated:YES completion:nil];return;
 }
 if([route isEqual:@"remasked-save-export"]){NSMutableArray* urls=[NSMutableArray array];for(NSString* name in @[@"save0.bin",@"save1.bin",@"save2.bin"]){NSString* path=Text((Root()/"saves"/name.UTF8String).string());if([NSFileManager.defaultManager fileExistsAtPath:path])[urls addObject:[NSURL fileURLWithPath:path]];}if(!urls.count){Notice(@"Saves",@"Start the game once to initialize your save slots.");return;}UIDocumentPickerViewController* p=[[UIDocumentPickerViewController alloc] initForExportingURLs:urls asCopy:YES];[self presentViewController:p animated:YES completion:nil];return;}
 if([route isEqual:@"remasked-save-import"]){self.pickerPurpose=@"saves";UIDocumentPickerViewController* p=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];p.allowsMultipleSelection=YES;p.delegate=self;[self presentViewController:p animated:YES completion:nil];return;}
}
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls{
 if ([self.pickerPurpose isEqual:@"prepared-game"]) {
    if (urls.count != 1 || GameRunning) return;
    NSURL* folder = urls.firstObject;
    const auto destination = PreparedContent();
    self.view.userInteractionEnabled = NO;
    self.status.text = @"Importing game files…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        const BOOL access = [folder startAccessingSecurityScopedResource];
        __block NSString* failure = nil;
        NSError* coordinationError = nil;
        NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [coordinator coordinateReadingItemAtURL:folder options:0 error:&coordinationError byAccessor:^(NSURL* source) {
            try {
                ImportPreparedGame(fs::path(source.fileSystemRepresentation), destination);
            } catch (const std::exception& error) {
                failure = Text(error.what());
            }
        }];
        if (access) [folder stopAccessingSecurityScopedResource];
        if (coordinationError) failure = @"Files could not open the selected folder.";
        dispatch_async(dispatch_get_main_queue(), ^{
            self.view.userInteractionEnabled = YES;
            if (failure) {
                self.status.text = @"Game import failed.";
                Notice(@"Game import failed", failure);
                return;
            }
            self.status.text = @"Game files imported. You can now start the game.";
            [self reload];
        });
    });
    return;
 }
 try{
 if([self.pickerPurpose isEqual:@"data"]){if(urls.count!=1)throw std::runtime_error("Choose one data folder");[self syncDrive:urls.firstObject resolution:ReMaskedDrive::Resolution::Ask];return;}
 if([self.pickerPurpose isEqual:@"model"]){if(urls.count!=1)throw std::runtime_error("Choose one prepared model folder");NSURL* selected=urls.firstObject;BOOL access=[selected startAccessingSecurityScopedResource];@try{
  namespace fs=std::filesystem;fs::path source(selected.fileSystemRepresentation);if(!fs::is_regular_file(source/"romfs/actors/zelda2_link_child_new.gar.lzs"))throw std::runtime_error("This is not a prepared MM3D player model folder");
  fs::path models=ReMaskedMenu::ModelsPath();fs::create_directories(models);fs::path stage=models/(".import-"+std::string(NSUUID.UUID.UUIDString.UTF8String));fs::create_directories(stage);
  try{uint64_t total=0;unsigned count=0;for(const auto& entry:fs::recursive_directory_iterator(source)){if(entry.is_symlink())throw std::runtime_error("Model folders cannot contain symbolic links");if(entry.is_directory())continue;if(!entry.is_regular_file())throw std::runtime_error("Unsupported model file");auto rel=entry.path().lexically_relative(source);if(*rel.begin()!="romfs"&&*rel.begin()!="texture-overrides"&&*rel.begin()!="voices"&&rel!="import.json"&&rel!="generated.json")continue;total+=entry.file_size();if(total>512ULL*1024*1024||++count>4096)throw std::runtime_error("Prepared model is too large");auto dest=stage/rel;fs::create_directories(dest.parent_path());fs::copy_file(entry.path(),dest);}
   if(fs::file_size(stage/"romfs/actors/zelda2_link_child_new.gar.lzs")<16)throw std::runtime_error("Prepared actor is empty");auto target=models/"pak-import";auto previous=models/("pak-import.previous-"+std::string(NSUUID.UUID.UUIDString.UTF8String));if(fs::exists(target))fs::rename(target,previous);try{fs::rename(stage,target);}catch(...){if(fs::exists(previous))fs::rename(previous,target);throw;}
   auto result=ReMaskedMenu::Save(Json{{"pak_link_model",true},{"adult_link_model",false},{"dark_link_model",false},{"twilight_link_model",false}},true);if(result.Error)throw std::runtime_error(result.Message);self.status.text=@"Prepared PAK model imported. It will load when you start the game.";NSLog(@"REMASKED_IOS_MODEL imported prepared actor files=%u",count);
  }catch(...){fs::remove_all(stage);throw;}
 }@finally{if(access)[selected stopAccessingSecurityScopedResource];}}
 else if([self.pickerPurpose isEqual:@"saves"]){if(GameRunning)throw std::runtime_error("Import saves from the startup screen after closing the game");if(urls.count<1||urls.count>3)throw std::runtime_error("Choose one to three native save slots");std::vector<std::pair<std::string,NSData*>> files;for(NSURL* url in urls){std::string name=url.lastPathComponent.UTF8String;if(name!="save0.bin"&&name!="save1.bin"&&name!="save2.bin")throw std::runtime_error("Use native save0.bin, save1.bin or save2.bin files");NSData* d=[NSData dataWithContentsOfURL:url];if(d.length!=79384)throw std::runtime_error("Invalid MM3D save size");files.push_back({name,d});}auto backup=Root()/"save-import-backups"/NSUUID.UUID.UUIDString.UTF8String;std::filesystem::create_directories(backup);for(auto& [name,data]:files){auto target=Root()/"saves"/name;if(std::filesystem::exists(target))std::filesystem::copy_file(target,backup/name);if(![data writeToFile:Text(target.string()) options:NSDataWritingAtomic error:nil])throw std::runtime_error("Save import failed");}self.status.text=@"Save slots imported; previous copies retained.";}
 [self reload];
 }catch(const std::exception& e){Notice(@"Import failed",Text(e.what()));}
}
@end
void ReMaskedIOSRoute(const std::string& route){OnMain(^{[ActiveController route:Text(route)];});}
void ReMaskedIOSAppendSettings(Menu::Document& document){
 ReMaskedMenu::Append(document,[](std::string page){ReMaskedIOSRoute(page);});
 for(auto& tab:document.Tabs){
  if(tab.Title=="Touch"){
    auto row=ReMaskedMenu::Toggle("portraitTouchArtwork","Portrait item artwork on controls",false,false,true);
    row.Help="Experimental: draw live item icons, counts and action text on the portrait touch controls. Off keeps the normal HUD inside the game. Hidden touch controls always restore the normal HUD.";
    tab.Rows.push_back(std::move(row));
  }
  std::erase_if(tab.Rows,[](const Menu::Row& r){return r.Key=="Show Select / App settings"||r.Key=="Select / App settings size"||r.Key=="Current graphics driver"||r.Key=="Use system driver"||r.Key=="Choose driver ZIP…"||r.Key=="Output resolution"||r.Key=="Screen mode"||r.Key=="Choose custom data folder…"||r.Key=="Use app data folder";});
  if(tab.Title=="Data"){
   Menu::Row setup;
   setup.Kind = Menu::RowKind::Action;
   setup.Key = "Import prepared game folder…";
   setup.Available = [] { return !GameRunning && !HasGameContent(); };
   setup.UnavailableReason = "Game files are already available.";
   setup.Activate = [] { ReMaskedIOSRoute("remasked-prepare-game"); return Menu::WriteResult{}; };
   tab.Rows.insert(tab.Rows.begin(), std::move(setup));
   Menu::Row location;location.Kind=Menu::RowKind::Info;location.Key="Data folder";location.DynamicText=[](){return std::string(ReMaskedDrive::FolderName().UTF8String);};tab.Rows.push_back(std::move(location));
   Menu::Row status;status.Kind=Menu::RowKind::Info;status.Key="Folder sync";status.DynamicText=[](){return std::string(ReMaskedDrive::Status().UTF8String);};tab.Rows.push_back(std::move(status));
   for(auto [label,route]:std::vector<std::pair<std::string,std::string>>{{"Choose iCloud Drive / Files folder…","remasked-data-folder"},{"Sync data folder now","remasked-data-sync"},{"Use local data only","remasked-data-default"}}){Menu::Row row;row.Kind=Menu::RowKind::Action;row.Key=label;row.Help="Sync saves, portable settings, imported models, textures and game files at startup and after quitting gameplay. Local working files and caches stay on this device. Deletions are not synced.";row.Available=[route](){return !GameRunning&&(route!="remasked-data-sync"||ReMaskedDrive::Enabled());};row.UnavailableReason=GameRunning?"Available from startup Settings after closing the game.":"Choose a data folder first.";row.Activate=[route](){ReMaskedIOSRoute(route);return Menu::WriteResult{};};tab.Rows.push_back(std::move(row));}
  }
  for(auto& row:tab.Rows){for(auto* text:{&row.Help,&row.Text,&row.UnavailableReason}){for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"Android’s file picker","Files"},{"Android launcher and recent-app icon","iOS Home Screen icon"},{"Android","iOS"},{"this driver","this device"}}){size_t pos=0;while((pos=text->find(pair.first,pos))!=std::string::npos){text->replace(pos,pair.first.size(),pair.second);pos+=pair.second.size();}}}
   if(row.Key=="Current data folder"){row.Key="Gameplay storage";row.DynamicText=[](){return std::string("On this device (local working copies)");};}
   if(row.Key=="App icon"){row.Help="Changes the ReMasked Home Screen icon.";}
   if(row.Key=="Aim source"){
    row.Help="Motion aiming uses a connected controller's gyro when available, otherwise the iPhone or iPad sensors. Rotate the device to aim; sensitivity applies to both.";
    for(auto& choice:row.Choices){auto at=choice.find("Controller");if(at!=std::string::npos)choice.replace(at,10,"Device / controller");}
   }
   if(row.Key=="Adult Link tunic")row.Help="Applies after fully closing and reopening ReMasked, so the selected model variant can be loaded.";
   if(row.Key=="PAK Model Loader"){row.Help="Import a model prepared on Android or desktop using Files. Raw PAK conversion is unavailable on iOS.";}
   if(GameRunning&&(row.Key=="Import saves"||row.Key=="Export saves"||row.Key=="PAK Model Loader")){row.Available=[](){return false;};row.UnavailableReason="Available from the startup screen after closing the game.";}
  }
 }
}
void ReMaskedIOSPresentSettings(InGameMenu& menu){
 if(!menu.IsOpen()||PresentingSettings)return;
 PresentingSettings=true;LiveMenu=&menu;const auto page=menu.Model().TabIndex();
 OnMain(^{
  UIViewController* parent=Presenter();ReMaskedIOSController* controller=[ReMaskedIOSController new];controller.settings=YES;controller.page=page;controller.modalPresentationStyle=UIModalPresentationFullScreen;ActiveController=controller;
  [parent presentViewController:controller animated:NO completion:nil];
 });
}
void ReMaskedIOSNativeMenuFrame(){
 if(PresentingSettings&&LiveMenu&&!LiveMenu->IsOpen()){
  [ActiveController dismissViewControllerAnimated:NO completion:nil];ActiveController=nil;LiveMenu=nullptr;PresentingSettings=false;
 }
}
void ReMaskedIOSApplyIcon(int index){
 if(index<0||index>4)return;
 OnMain(^{if(!UIApplication.sharedApplication.supportsAlternateIcons)return;
 [UIApplication.sharedApplication setAlternateIconName:index==0?nil:[NSString stringWithFormat:@"AppIcon%d",index] completionHandler:^(NSError* error){if(error)OnMain(^{Notice(@"App icon",error.localizedDescription);});}];});
}
void ReMaskedIOSLaunch(){
 if(GameRunning||LauncherShown)return;
 // Explicit developer preflight modes bypass the launcher; ordinary users never do.
 if(const char* skip=std::getenv("REMASKED_IOS_SKIP_LAUNCHER");skip&&std::string_view(skip)=="1"){GameRunning=true;return;}
 std::filesystem::create_directories(Root()/"models");
 // Hash-verify private prepared built-ins before first-use copying. Never replace
 // a user's imported PAK or a modified model folder.
 NSURL* bundled=[NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"PreparedModels"];
 NSData* manifestData=[NSData dataWithContentsOfURL:[bundled URLByAppendingPathComponent:@"manifest.json"]];
 if(manifestData){NSDictionary* manifest=[NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil];
  for(NSString* name in manifest){NSData* bytes=[NSData dataWithContentsOfURL:[bundled URLByAppendingPathComponent:name]];unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,digest);NSMutableString* hash=[NSMutableString string];for(auto b:digest)[hash appendFormat:@"%02x",b];if(![hash isEqual:manifest[name]])throw std::runtime_error("Bundled model failed verification");
   auto target=Root()/"models"/name.UTF8String;if(!std::filesystem::exists(target)){std::filesystem::create_directories(target.parent_path());if(![bytes writeToFile:Text(target.string()) options:NSDataWritingAtomic error:nil])throw std::runtime_error("Could not initialize built-in models");}
  }
 }
 setenv("REMASKED_MODELS_ROOT",(Root()/"models").c_str(),1);
 // Register the same preview images used by the shared in-game settings.
 for(int i=0;i<5;++i){UIImage* image=[UIImage imageNamed:i==0?@"ReMaskedLauncher.png":[NSString stringWithFormat:@"ReMaskedIcon%d.png",i]];
  if(!image.CGImage)continue;oot3d::ui::UiTexturePixels p;p.width=128;p.height=128;p.rgba8.resize(128*128*4);
  CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();CGContextRef c=CGBitmapContextCreate(p.rgba8.data(),128,128,8,128*4,space,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);CGContextDrawImage(c,CGRectMake(0,0,128,128),image.CGImage);CGContextRelease(c);CGColorSpaceRelease(space);Menu::RegisterMenuImage("menu/image/remasked-icon/"+std::to_string(i),std::move(p));
 }
 auto controls=NativeControlDefaults();std::string error;auto cp=Root()/"oot3d_controls.json";if(std::filesystem::exists(cp)){if(!LoadNativeControlConfig(cp,&controls,&error))throw std::runtime_error(error);}else{ReMasked::MigrateDefaultShoulders(controls);if(!SaveNativeControlConfig(cp,controls,&error))throw std::runtime_error(error);}
 const auto normalizedAim=ReMaskedIOS::NormalizeAimMode(controls.NativeAimSource);
 if(normalizedAim!=controls.NativeAimSource){controls.NativeAimSource=normalizedAim;if(!SaveNativeControlConfig(cp,controls,&error))throw std::runtime_error(error);}
 std::string deviceKey="iOS-"+std::string(UIDevice.currentDevice.model.UTF8String)+"-"+UIDevice.currentDevice.systemVersion.UTF8String;
 setenv("REMASKED_GRAPHICS_DEVICE_KEY",deviceKey.c_str(),1);setenv("REMASKED_GRAPHICS_CAPABILITIES",(Root()/"graphics-capabilities.json").c_str(),1);
 auto store=std::make_shared<Store>(Root()/"native-game.json");if(!std::filesystem::exists(store->Path))ReMaskedMenu::AtomicWrite(store->Path,Json::object());Fast::Extension::InstallGraphicsSettingsPersistencePort(store);ReMaskedGraphics::Load();
 for (const auto* name : {"presentation.json", "runtime-presentation.json"}) {
    if (!fs::exists(Root() / name)) ReMaskedMenu::AtomicWrite(Root() / name, Json::object());
 }
 if(!Mm3dSingleScreen::LoadSettings(Root()/"presentation.json"))throw std::runtime_error("Could not load ReMasked presentation settings");
 NSData* saved=[NSUserDefaults.standardUserDefaults dataForKey:@"ReMasked.TouchSettings.v1"];if(saved)try{ReMaskedMenu::ReportTouch(Json::parse(std::string((const char*)saved.bytes,saved.length)));}catch(...){}
 {
  auto& state=ReMaskedMenu::Touch();std::lock_guard lock(state.Mutex);auto& v=state.Values;
  ThreeDsRecomp::Input::OverlayTouchSettings().Report({v.value("visible",true),v.value("controlOpacity",28),v.value("controlScale",50)+50,v.value("vibration",true),v.value("hideWithController",true)});
  v["dataDirectory"]=Root().string();
 }
 std::shared_ptr<GameLanguageSettings> language;
 // Standalone Settings is available before any game content has been imported.
 const auto bundledGame = BundledContent();
 if (fs::is_regular_file(bundledGame / ReMaskedIOS::PreparedGameManifest)) {
    const auto source = ReMaskedMenu::ReadFile(bundledGame / ReMaskedIOS::PreparedGameManifest).at("source");
    auto languages = DetectGameLanguages(bundledGame / source.at("romfs_image_path").get<std::string>(),
        source.value("romfs_service_offset", uint64_t(0)), source.value("romfs_service_size", uint64_t(0)), TitleAdapter::Mm3d);
    language = std::make_shared<GameLanguageSettings>(Root() / "game_language.json", languages);
 }
 auto runtime=std::make_shared<NativeControlConfigRuntime>(cp,controls);InGameMenuHost host;host.Controls=Menu::HelpControls::Touch;host.SaveDataDirectory=Root()/"saves";host.LogsDirectory=Root()/"logs";host.OpenInstallerPage=[](std::string_view route,std::string*){ReMaskedIOSRoute(std::string(route));return true;};host.AppendSettingsTabs=[](Menu::Document& d){ReMaskedIOSAppendSettings(d);};
 Startup=std::make_unique<InGameMenu>(InGameMenuOwners{runtime,nullptr,language,nullptr,nullptr,nullptr,std::move(host),std::make_shared<Ship::AudioOutputPolicy>(store),{.QuickPutAway=true,.RollDash=true},true});
 for(auto& tab:SettingsModel().Doc().Tabs){std::erase_if(tab.Rows,[](const Menu::Row& r){return r.Key=="Quit game";});for(auto& row:tab.Rows){if(row.Key=="Edit layout"||row.Key=="Calibrate controller gyro"){row.Available=[](){return false;};row.UnavailableReason="Available during gameplay";}if(tab.Title=="Graphics"&&row.Write){auto write=row.Write;row.Write=[write,store](int value){auto result=write(value);if(!result.Error&&!store->StoreGraphics(Fast::Extension::SerializeGraphicsSettings(Fast::Extension::GraphicsSettingsRuntime::Instance().Snapshot())))return Menu::WriteResult{"Could not save graphics",true};return result;};}}}
 UIWindowScene* scene=nil;for(UIScene* s in UIApplication.sharedApplication.connectedScenes)if([s isKindOfClass:UIWindowScene.class]){scene=(UIWindowScene*)s;break;}if(!scene)throw std::runtime_error("No connected iPhone scene");
 UIWindow* window=[[UIWindow alloc] initWithWindowScene:scene];window.frame=scene.coordinateSpace.bounds;ReMaskedIOSController* controller=[ReMaskedIOSController new];ActiveController=controller;window.rootViewController=controller;[window makeKeyAndVisible];NSLog(@"REMASKED_IOS_LAUNCHER ready; renderer not started");
 LauncherWindow=window;LauncherShown=true;
 if(!DriveChecked&&ReMaskedDrive::Enabled()){DriveChecked=true;dispatch_async(dispatch_get_main_queue(),^{[controller syncDrive:nil resolution:ReMaskedDrive::Resolution::Ask];});}
 __block bool displaySettingsAdded=false;
 controller.layoutObserver=[[ReMaskedDuoLayoutObserver alloc] initWithView:controller.view handler:^(ReMaskedDuoLayoutSnapshot* snapshot){
  // Native UIKit sheets own their layout. Startup only observes hinge geometry;
  // SDL window sizing and the lower-screen touch view belong to gameplay.
  if(controller.presentedViewController)return;
  auto rect=[](CGRect r){return ReMaskedDuo::Rect{float(r.origin.x),float(r.origin.y),float(r.size.width),float(r.size.height)};};
  ReMaskedDuo::Geometry g;g.Available=snapshot.phoneWithHinge;g.Closed=snapshot.hingeStatus==0||snapshot.hingeStatus==1;g.Bounds=rect(snapshot.bounds);g.Safe=rect(snapshot.safeBounds);
  for(NSValue* value in snapshot.divisions){g.Division=rect(value.CGRectValue);if(ReMaskedDuo::Divide(g).Dual())break;}
  for(NSValue* value in snapshot.occlusions)g.Occlusions.push_back(rect(value.CGRectValue));
  ReMaskedDuo::GameDisplay.Publish(std::move(g));
  if(!displaySettingsAdded&&snapshot.phoneWithHinge){displaySettingsAdded=true;ReMaskedAppleAppendDisplaySettings(SettingsModel().Doc());[controller reload];}
 }];
 LauncherTimer=[NSTimer scheduledTimerWithTimeInterval:.2 repeats:YES block:^(NSTimer*){[controller.layoutObserver refresh];}];
}
namespace {
void RebuildStartup(){
 bool settings=ActiveController.settings;NSInteger page=ActiveController.page;
 // Canonical portable HUD/model settings are projected into the local runtime file.
 auto canonical=Root()/"presentation.json";if(std::filesystem::is_regular_file(canonical))ReMaskedMenu::AtomicWrite(Root()/"runtime-presentation.json",ReMaskedMenu::ReadFile(canonical));
 [ActiveController.layoutObserver invalidate];[LauncherTimer invalidate];LauncherTimer=nil;
 LauncherWindow.hidden=YES;LauncherWindow.rootViewController=nil;LauncherWindow=nil;ActiveController=nil;Startup.reset();LauncherShown=false;
 ReMaskedIOSLaunch();ActiveController.settings=settings;ActiveController.page=page;[ActiveController reload];
}
void FinishStartupLaunch(){
 // Apply file-backed model selection only before a guest filesystem exists.
 auto j=ReMaskedMenu::Current(true);
 auto profile=Root()/"ReMaskedNativeGame.launch.json";
 Json launch;
 if (fs::is_regular_file(PreparedContent() / ReMaskedIOS::PreparedGameManifest)) {
    ValidatePreparedGame(PreparedContent());
    launch = ReMaskedIOS::PreparedGameLaunchProfile();
 } else {
    launch = ReMaskedMenu::ReadFile(profile);
 }
 auto args=launch.at("arguments");
 for(int i=int(args.size())-2;i>=0;--i)if(args[i]=="--title-romfs-layer"&&args[i+1].is_string()&&args[i+1].get<std::string>().starts_with((Root()/"models").string())){args.erase(args.begin()+i,args.begin()+i+2);}
 for(const char* key:{"REMASKED_GAME_MODEL_LAYER","REMASKED_GAME_VOICE_LAYER","REMASKED_GAME_MODEL_TEXTURES","REMASKED_SWORD_ICON"})unsetenv(key);
 setenv("REMASKED_TALL_PLAYER","0",1);setenv("REMASKED_ADULT_TUNIC_COLOR","0",1);
 for(const auto& [key,folder]:std::vector<std::pair<std::string,std::string>>{{"pak_link_model","pak-import"},{"twilight_link_model","twilight-link"},{"dark_link_model","dark-link"},{"adult_link_model","adult-link"}})if(j.value(key,false)){
  auto installed=Root()/"models"/folder,layer=installed/"romfs";
  if(folder=="adult-link"||folder=="twilight-link"){
   bool twilight=folder=="twilight-link";std::string sword=j.value(twilight?"twilight_link_master_sword":"adult_link_master_sword",twilight)?"master":"kokiri";
   int color=twilight?0:j.value("adult_link_tunic_color",0);std::string suffix=color==1?"-blue":color==2?"-red":"";
   for(auto path:{"variants/"+sword+suffix+"/romfs","variants/"+sword+"/romfs",std::string("romfs")})if(std::filesystem::is_regular_file(installed/path/"actors/zelda2_link_child_new.gar.lzs")){layer=installed/path;break;}
   setenv("REMASKED_ADULT_TUNIC_COLOR",std::to_string(color).c_str(),1);
  }
  if(!std::filesystem::is_regular_file(layer/"actors/zelda2_link_child_new.gar.lzs"))throw std::runtime_error("Selected model is missing; import it from Models first");
  setenv("REMASKED_GAME_MODEL_LAYER",layer.c_str(),1);bool tall=folder!="pak-import";
  if(!tall&&std::filesystem::is_regular_file(installed/"import.json"))tall=ReMasked::TallPakModel(ReMaskedMenu::ReadFile(installed/"import.json"));
  setenv("REMASKED_TALL_PLAYER",tall?"1":"0",1);
  if(std::filesystem::is_directory(installed/"texture-overrides"))setenv("REMASKED_GAME_MODEL_TEXTURES",(installed/"texture-overrides").c_str(),1);
  if(std::filesystem::is_regular_file(installed/"voices/romfs/sound/sequence/JokerSound.bcsar"))setenv("REMASKED_GAME_VOICE_LAYER",(installed/"voices/romfs").c_str(),1);
  if(std::filesystem::is_regular_file(installed/"master-sword-icon.png")&&(folder=="dark-link"||layer.string().find("/variants/master")!=std::string::npos))setenv("REMASKED_SWORD_ICON",(installed/"master-sword-icon.png").c_str(),1);
  break;
 }
 launch["arguments"]=args;ReMaskedMenu::AtomicWrite(profile,launch);
 SaveTouch();Startup.reset();Fast::Extension::InstallGraphicsSettingsPersistencePort(nullptr);GameRunning=true;[ActiveController.layoutObserver invalidate];ActiveController.layoutObserver=nil;ActiveController=nil;[LauncherTimer invalidate];LauncherTimer=nil;LauncherWindow.hidden=YES;LauncherWindow.rootViewController=nil;LauncherWindow=nil;
}

} // namespace
void InitializeReMaskedAppleGameHost();
extern "C" int ReMaskedIOSDeferMain(void (*resume)()){
 if(GameRunning)return 0;
 for(const char* key:{"REMASKED_IOS_SKIP_LAUNCHER","REMASKED_SAVE_SYNC_PREFLIGHT"})
  if(const char* value=std::getenv(key);value&&std::string_view(value)=="1")return 0;
 ResumeMain=resume;
 try{InitializeReMaskedAppleGameHost();ReMaskedIOSLaunch();}
 catch(const std::exception& error){NSLog(@"REMASKED_IOS_LAUNCHER_ERROR %s",error.what());}
 return 1;
}
extern "C" void ReMaskedIOSDidFinishGame(){
 [ActiveController dismissViewControllerAnimated:NO completion:nil];
 ReMaskedIOS_StopTouchControls();ReMaskedDual::Active=false;ReMaskedDual::Touch=0;
 LiveMenu=nullptr;PresentingSettings=false;ActiveController=nil;
 GameRunning=false;StartRequested=false;LauncherShown=false;DriveChecked=false;
 ThreeDsRecomp::Input::TouchOverlayPad().ReleaseAll();
 ReMaskedMenu::Visible=false;
 ReMaskedIOSLaunch();
}
