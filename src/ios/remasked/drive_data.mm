// SPDX-License-Identifier: GPL-3.0-or-later
#import "drive_data.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdexcept>
#include <cstdio>
namespace ReMaskedDrive {
namespace {
NSString* const Bookmark=@"ReMasked.DataFolder.Bookmark.v1";
NSString* const Label=@"ReMasked.DataFolder.Name.v1";
NSString* state=@"Local working files; no Drive folder selected.";
bool busy=false;
void Require(bool ok, NSString* message){if(!ok)throw std::runtime_error(message.UTF8String);}
NSString* Hash(NSData* data){unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);NSMutableString* s=[NSMutableString string];for(auto b:digest)[s appendFormat:@"%02x",b];return s;}
NSString* Digest(NSURL* url){
 NSInputStream* stream=[NSInputStream inputStreamWithURL:url];[stream open];CC_SHA256_CTX context;CC_SHA256_Init(&context);uint8_t bytes[65536];NSInteger read;
 while((read=[stream read:bytes maxLength:sizeof(bytes)])>0)CC_SHA256_Update(&context,bytes,(CC_LONG)read);
 [stream close];Require(read==0,@"A file could not be read. Download it in Files and try again.");unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&context);NSMutableString* s=[NSMutableString string];for(auto b:digest)[s appendFormat:@"%02x",b];return s;
}
NSDictionary<NSString*, NSString*>* Directories(Content content) {
    if (content == Content::AzaharData)
        return @{@"saves":AzaharSavesPath(), @"models":@"Models",
                 @"textures":@"Azahar/load/textures/0004000000125500"};
    if (content != Content::Native) return @{@"saves":AzaharSavesPath()};
    return @{@"saves":@"saves", @"models":@"models", @"textures/load":@"textures/load",
             @"GameContent/PrivateGame":@"GameContent/PrivateGame",
             @"GameContent/RestorationHud":@"GameContent/RestorationHud"};
}
NSString* const AzaharSaveMetadata = @"saves/00000001.metadata";
NSURL* Resolve(NSURL* local, NSURL* drive, NSString* name, bool localSide, Content content) {
    if (!localSide) return [drive URLByAppendingPathComponent:name];
    if (content != Content::Native && [name isEqualToString:AzaharSaveMetadata])
        return [local URLByAppendingPathComponent:[AzaharSavesPath() stringByAppendingPathExtension:@"metadata"]];
    for (NSString* key in Directories(content)) {
        NSString* prefix=[key stringByAppendingString:@"/"];
        if ([name hasPrefix:prefix])
            return [[local URLByAppendingPathComponent:Directories(content)[key]]
                    URLByAppendingPathComponent:[name substringFromIndex:prefix.length]];
    }
    return [local URLByAppendingPathComponent:name];
}
NSArray<NSString*>* Settings(){return @[@"presentation.json",@"game_language.json",@"oot3d_controls.json"];}
void Regular(NSURL* url, bool directory){NSDictionary* a=[NSFileManager.defaultManager attributesOfItemAtPath:url.path error:nil];Require([a[NSFileType] isEqual:(directory?NSFileTypeDirectory:NSFileTypeRegular)],@"Data folders cannot contain symbolic links or special files.");}
bool IsClassicCycle(NSURL* local, NSString* name, NSString* digest, Content content) {
    if (content == Content::Native || !digest ||
        ![@[@"saves/save0.bin", @"saves/save1.bin", @"saves/save2.bin"] containsObject:name]) return false;
    NSURL* slot = Resolve(local, local, name, true, content);
    NSURL* sidecars = [slot.URLByDeletingLastPathComponent URLByAppendingPathComponent:@".classic-saving"
                                                                        isDirectory:YES];
    NSURL* receipt = [sidecars URLByAppendingPathComponent:[slot.lastPathComponent
                                                           stringByAppendingString:@".cycle"]];
    if (![NSFileManager.defaultManager fileExistsAtPath:receipt.path]) return false;
    Regular(sidecars, true);
    Regular(receipt, false);
    return [Digest(receipt) isEqualToString:digest];
}
NSDictionary* Scan(NSURL* root, Content content, bool localSide){
 NSFileManager* fm=NSFileManager.defaultManager;Regular(root,true);NSMutableDictionary* hashes=[NSMutableDictionary dictionary];__block uint64_t total=0;__block NSUInteger count=0;
 void (^add)(NSURL*,NSString*)=^(NSURL* url,NSString* relative){Regular(url,false);NSNumber* size=nil;[url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];total+=size.unsignedLongLongValue;Require(++count<=100000&&total<=32ULL*1024*1024*1024,@"The selected data exceeds the 32 GiB or file-count limit.");if([Settings() containsObject:relative]){Require(size.unsignedLongLongValue<=2*1024*1024,@"A settings file is too large.");id document=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:url] options:0 error:nil];Require([document isKindOfClass:NSDictionary.class],@"A settings file is invalid. Neither copy was changed.");}if(content!=Content::Native&&[relative isEqualToString:AzaharSaveMetadata]) {
    Require(size.unsignedLongLongValue==16,@"The Azahar save archive metadata is invalid.");
   }else if([relative hasPrefix:@"saves/"]){Require([@[@"saves/save0.bin",@"saves/save1.bin",@"saves/save2.bin"] containsObject:relative]&&size.unsignedLongLongValue==79384,@"The folder contains an unsupported or invalid native save.");}hashes[relative]=Digest(url);};
 if(content==Content::Native)for(NSString* relative in Settings()){NSURL* url=[root URLByAppendingPathComponent:relative];if([fm fileExistsAtPath:url.path])add(url,relative);}
 NSDictionary* directories=Directories(content);
 for(NSString* key in directories){
  NSString* directory=localSide?directories[key]:key;
  // Validate each ancestor as well (textures could itself be a symlink).
  NSURL* current=root;for(NSString* part in directory.pathComponents){current=[current URLByAppendingPathComponent:part];if([fm fileExistsAtPath:current.path])Regular(current,true);}
  NSURL* base=[root URLByAppendingPathComponent:directory];if(![fm fileExistsAtPath:base.path])continue;
  __block NSError* enumerationError=nil;NSDirectoryEnumerator* it=[fm enumeratorAtURL:base includingPropertiesForKeys:nil options:0 errorHandler:^BOOL(NSURL*,NSError* error){enumerationError=error;return NO;}];
  for(NSURL* url in it){NSDictionary* a=[fm attributesOfItemAtPath:url.path error:nil];if([a[NSFileType] isEqual:NSFileTypeDirectory])continue;Regular(url,false);
   NSArray* prefix=root.URLByResolvingSymlinksInPath.path.pathComponents;NSArray* parts=url.URLByResolvingSymlinksInPath.path.pathComponents;
   Require(parts.count>prefix.count&&[[parts subarrayWithRange:NSMakeRange(0,prefix.count)] isEqual:prefix],@"Data file escaped the selected folder.");
   NSString* relative=[[parts subarrayWithRange:NSMakeRange(prefix.count,parts.count-prefix.count)] componentsJoinedByString:@"/"];
   if (content != Content::Native) {
    if ([key isEqualToString:@"saves"]) {
     NSString* name=url.lastPathComponent;
     if (![@[@"save0.bin",@"save1.bin",@"save2.bin"] containsObject:name] ||
         ![relative isEqualToString:[directory stringByAppendingPathComponent:name]]) continue;
    } else if ([url.lastPathComponent hasPrefix:@"."]) continue;
   }
   add(url,[key stringByAppendingString:[relative substringFromIndex:directory.length]]);
  }
  Require(!enumerationError,@"The data folder could not be enumerated.");
 }
 if(content!=Content::Native) {
  NSURL* metadata=Resolve(root,root,AzaharSaveMetadata,localSide,content);
  if([fm fileExistsAtPath:metadata.path])add(metadata,AzaharSaveMetadata);
 }
 return hashes;
}
}
NSString* FolderName(){return [NSUserDefaults.standardUserDefaults stringForKey:Label]?:@"Not selected (local only)";}
NSString* Status(){return state;}
bool Enabled(){return [NSUserDefaults.standardUserDefaults dataForKey:Bookmark]!=nil;}
void Disconnect(){[NSUserDefaults.standardUserDefaults removeObjectForKey:Bookmark];[NSUserDefaults.standardUserDefaults removeObjectForKey:Label];state=@"Using local data. The Drive copy was retained.";}
Result Reconcile(NSURL* local,NSURL* folder,Resolution resolution,Content content){
 @try {try {
  NSFileManager* fm=NSFileManager.defaultManager;Regular(local,true);Regular(folder,true);
  local=local.URLByResolvingSymlinksInPath;folder=folder.URLByResolvingSymlinksInPath;
  NSString* lp=local.URLByStandardizingPath.path;NSString* fp=folder.URLByStandardizingPath.path;
  Require(![lp isEqual:fp]&&![lp hasPrefix:[fp stringByAppendingString:@"/"]]&&![fp hasPrefix:[lp stringByAppendingString:@"/"]],@"Choose a folder outside the app's working data.");
  NSDictionary* left=Scan(local,content,true);NSDictionary* right=Scan(folder,content,false);
  NSURL* metadata=[local URLByAppendingPathComponent:@".drive-sync" isDirectory:YES];
  if([fm fileExistsAtPath:metadata.path])Regular(metadata,true);else Require([fm createDirectoryAtURL:metadata withIntermediateDirectories:NO attributes:nil error:nil],@"Could not create local sync metadata.");
  NSString* key=Hash([fp dataUsingEncoding:NSUTF8StringEncoding]);NSURL* baseline=[metadata URLByAppendingPathComponent:[key stringByAppendingString:@".plist"]];
  if([fm fileExistsAtPath:baseline.path])Regular(baseline,false);
  NSDictionary* previous=[NSDictionary dictionaryWithContentsOfURL:baseline]?:@{};
  NSMutableSet* names=[NSMutableSet setWithArray:left.allKeys];[names addObjectsFromArray:right.allKeys];
  NSMutableArray* changes=[NSMutableArray array];NSMutableDictionary* merged=[previous mutableCopy];
  bool pendingImport=false, protectedCycle=false;
  for(NSString* name in [[names allObjects] sortedArrayUsingSelector:@selector(compare:)]){
   NSString* l=left[name];NSString* r=right[name];NSString* old=previous[name];bool toLocal=false;
   // A managed cycle slot is the only cloud-visible classic checkpoint.
   // A different remote revision must never be installed over it.
   if(r && ![r isEqual:l] && ![r isEqual:old] && IsClassicCycle(local,name,l,content)) {
       protectedCycle=true;
       continue;
   }
   if (content == Content::AzaharCheck) {
       if (![name isEqualToString:AzaharSaveMetadata] && r &&
           ![r isEqual:l] && ![r isEqual:old])
           pendingImport = true;
       continue;
   }
   if(content==Content::AzaharBackup && r && (!l || (![r isEqual:l] && ![r isEqual:old]))) {
       pendingImport=true;
       continue;
   }
   if(l&&r&&![l isEqual:r]){
    if(old&&[l isEqual:old])toLocal=true;
    else if(old&&[r isEqual:old])toLocal=false;
    else {if(resolution==Resolution::Ask)return {false,true,@"Both copies differ. Choose which version to keep for conflicting files."};toLocal=resolution==Resolution::Drive;}
   }else if(!l)toLocal=true;
   NSString* desired=toLocal?r:l;merged[name]=desired;
   if(l&&r&&[l isEqual:r])continue;
   [changes addObject:@{@"name":name,@"local":@(toLocal),@"digest":desired}];
  }
  if (content == Content::AzaharCheck) {
      if (pendingImport) return {true,false,@"Newer saves are available in iCloud Drive.",true};
      if (protectedCycle) return {true,false,@"Kept the local permanent Classic Saving checkpoint."};
      return {true,false,@"No newer iCloud saves found."};
  }
  // Stage every input before replacing anything. Memory stays bounded even for
  // ROMFS images and texture packs. No deletion propagation in either direction.
  NSURL* stage=[metadata URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];Require([fm createDirectoryAtURL:stage withIntermediateDirectories:NO attributes:nil error:nil],@"Could not stage data sync.");
  NSMutableArray* committed=[NSMutableArray array];
  NSURL* saveBackups=[[local URLByAppendingPathComponent:@"Save Backups" isDirectory:YES]
      URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
  try {
   NSUInteger index=0;
   for(NSDictionary* change in changes){NSURL* source=Resolve(local,folder,change[@"name"],![change[@"local"] boolValue],content);NSURL* copy=[stage URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu.new",(unsigned long)index++]];Require([fm copyItemAtURL:source toURL:copy error:nil],@"Could not download or stage a file. Local data is retained.");Require([Digest(copy) isEqual:change[@"digest"]],@"A file changed during sync. Try again.");}
   index=0;
   for(NSDictionary* change in changes){NSURL* target=Resolve(local,folder,change[@"name"],[change[@"local"] boolValue],content);NSURL* parent=[target URLByDeletingLastPathComponent];Require([fm createDirectoryAtURL:parent withIntermediateDirectories:YES attributes:nil error:nil],@"Could not create a data directory.");NSURL* copy=[stage URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu.new",(unsigned long)index]];NSURL* backup=[stage URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu.old",(unsigned long)index++]];
    BOOL exists=[fm fileExistsAtPath:target.path];if(exists){
     Require([fm copyItemAtURL:target toURL:backup error:nil],@"Could not preserve the previous file.");
     if(content!=Content::Native&&[change[@"local"] boolValue]&&[change[@"name"] hasPrefix:@"saves/"]) {
      Require([fm createDirectoryAtURL:saveBackups withIntermediateDirectories:YES attributes:nil error:nil],
          @"Could not create a backup for the local save.");
      Require([fm copyItemAtURL:backup toURL:[saveBackups URLByAppendingPathComponent:[change[@"name"] lastPathComponent]] error:nil],
          @"Could not back up the local save.");
     }
    }
    // NSData's atomic write would load large texture/ROM files into memory.
    // Copy to an adjacent temp and rename instead, including cross-volume sync.
    NSURL* adjacent=[parent URLByAppendingPathComponent:[@".remasked-sync-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    Require([fm copyItemAtURL:copy toURL:adjacent error:nil],@"Could not write the data file.");
    int result=rename(adjacent.fileSystemRepresentation,target.fileSystemRepresentation);if(result!=0){[fm removeItemAtURL:adjacent error:nil];throw std::runtime_error("Could not commit a data file");}
    [committed addObject:@{@"target":target,@"backup":backup,@"existed":@(exists)}];
   }
   Require([merged writeToURL:baseline atomically:YES],@"Could not save the sync baseline.");
  }catch(...){for(NSDictionary* entry in committed.reverseObjectEnumerator){NSURL* target=entry[@"target"];if([entry[@"existed"] boolValue]){[fm removeItemAtURL:target error:nil];[fm copyItemAtURL:entry[@"backup"] toURL:target error:nil];}else [fm removeItemAtURL:target error:nil];}throw;}
  [fm removeItemAtURL:stage error:nil];
  if(protectedCycle)return {true,false,@"Kept the permanent Classic Saving checkpoint. A different Drive copy was left in place."};
  if(pendingImport)return {true,false,@"Drive has save changes. They can be synced from startup Settings."};
  return {true,false,content==Content::AzaharSaves||content==Content::AzaharBackup?
      @"Save slots synced with the selected folder.":@"Drive folder is up to date. Gameplay uses local working files."};
 }catch(const std::exception& e){return {false,false,[NSString stringWithUTF8String:e.what()]};}}
 @catch(NSException* exception){return {false,false,@"The folder could not be synced. Local data is retained."};}
}
void Sync(NSURL* local,NSURL* selected,Resolution resolution,void (^completion)(Result),Content content){
 if(busy){completion({false,false,@"Data sync is already running."});return;}busy=true;state=@"Syncing data folder…";
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool{
  if (content != Content::Native) {
   NSFileManager* files=NSFileManager.defaultManager;
   NSURL* container=[files URLForUbiquityContainerIdentifier:AzaharContainerIdentifier()];
   NSURL* documents=[container URLByAppendingPathComponent:@"Documents" isDirectory:YES];
   NSError* error=nil;
   __block Result result{false,false,@"iCloud Drive is unavailable. Local files are available."};
   if(documents&&[files createDirectoryAtURL:documents withIntermediateDirectories:YES attributes:nil error:&error]) {
    NSFileCoordinator* coordinator=[[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateWritingItemAtURL:documents options:0 error:&error byAccessor:^(NSURL* coordinated) {
     // The document makes the app-owned folder visible in Files even before
     // the first game save. Only the sync allowlist is reconciled.
     NSURL* readme=[coordinated URLByAppendingPathComponent:@"ReMasked Data.txt"];
     if(![files fileExistsAtPath:readme.path]) {
      NSString* instructions=@"ReMasked Azahar\n\n"
       "Place MM3D texture packs in textures and prepared models in models. "
       "Use Settings > Data > Sync data folder now to apply changes. "
       "The saves folder is maintained by ReMasked. ROMs and caches stay on the device.\n";
      if(![instructions writeToURL:readme atomically:YES encoding:NSUTF8StringEncoding error:nil]){
       result={false,false,@"Could not create the app folder in iCloud Drive. Local data is retained."};
       return;
      }
     }
     result=Reconcile(local,coordinated,resolution,content);
    }];
    if(error)result={false,false,@"The app's iCloud Drive folder is unavailable. Local data is retained."};
   }
   dispatch_async(dispatch_get_main_queue(),^{busy=false;state=result.message;completion(result);});
   return;
  }
  __block Result result;NSURL* url=selected;BOOL stale=NO;NSError* error=nil;NSData* bookmark=nil;
  if(!url){bookmark=[NSUserDefaults.standardUserDefaults dataForKey:Bookmark];url=bookmark?[NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:&error]:nil;}
  BOOL access=url?[url startAccessingSecurityScopedResource]:NO;
  if(!url||stale)result={false,false,@"Choose the data folder again in Files. Local data is available."};
  else {
   // Always use a dedicated subdirectory; never reconcile unrelated user files.
   NSFileCoordinator* coordinator=[[NSFileCoordinator alloc] initWithFilePresenter:nil];
   [coordinator coordinateWritingItemAtURL:url options:0 error:&error byAccessor:^(NSURL* coordinated){
    NSURL* data=[coordinated URLByAppendingPathComponent:@"Majora ReMasked" isDirectory:YES];NSError* mkdirError=nil;
    if(![NSFileManager.defaultManager createDirectoryAtURL:data withIntermediateDirectories:NO attributes:nil error:&mkdirError]&&mkdirError.code!=NSFileWriteFileExistsError){result={false,false,@"The selected folder is unavailable. Local data is retained."};return;}
    result=Reconcile(local,data,resolution,content);
   }];
   if(error)result={false,false,@"The selected folder is unavailable. Local data is retained."};
   if(result.ok&&selected){NSData* saved=[selected bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:&error];if(saved){[NSUserDefaults.standardUserDefaults setObject:saved forKey:Bookmark];[NSUserDefaults.standardUserDefaults setObject:selected.lastPathComponent forKey:Label];}else result={false,false,@"Files synced, but folder access could not be remembered. Choose it again."};}
  }
  if(access)[url stopAccessingSecurityScopedResource];
  dispatch_async(dispatch_get_main_queue(),^{busy=false;state=result.message;completion(result);});
 }});
}
}
