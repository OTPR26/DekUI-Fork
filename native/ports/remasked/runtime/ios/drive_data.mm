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
NSArray<NSString*>* Directories(){return @[@"saves",@"models",@"textures/load",@"GameContent/PrivateGame",@"GameContent/RestorationHud"] ;}
NSArray<NSString*>* Settings(){return @[@"presentation.json",@"game_language.json",@"oot3d_controls.json"];}
void Regular(NSURL* url, bool directory){NSDictionary* a=[NSFileManager.defaultManager attributesOfItemAtPath:url.path error:nil];Require([a[NSFileType] isEqual:(directory?NSFileTypeDirectory:NSFileTypeRegular)],@"Data folders cannot contain symbolic links or special files.");}
NSDictionary* Scan(NSURL* root){
 NSFileManager* fm=NSFileManager.defaultManager;Regular(root,true);NSMutableDictionary* hashes=[NSMutableDictionary dictionary];__block uint64_t total=0;__block NSUInteger count=0;
 void (^add)(NSURL*,NSString*)=^(NSURL* url,NSString* relative){Regular(url,false);NSNumber* size=nil;[url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];total+=size.unsignedLongLongValue;Require(++count<=100000&&total<=32ULL*1024*1024*1024,@"The selected data exceeds the 32 GiB or file-count limit.");if([Settings() containsObject:relative]){Require(size.unsignedLongLongValue<=2*1024*1024,@"A settings file is too large.");id document=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:url] options:0 error:nil];Require([document isKindOfClass:NSDictionary.class],@"A settings file is invalid. Neither copy was changed.");}if([relative hasPrefix:@"saves/"]){Require([@[@"saves/save0.bin",@"saves/save1.bin",@"saves/save2.bin"] containsObject:relative]&&size.unsignedLongLongValue==79384,@"The folder contains an unsupported or invalid native save.");}hashes[relative]=Digest(url);};
 for(NSString* relative in Settings()){NSURL* url=[root URLByAppendingPathComponent:relative];if([fm fileExistsAtPath:url.path])add(url,relative);}
 for(NSString* directory in Directories()){
  // Validate each ancestor as well (textures could itself be a symlink).
  NSURL* current=root;for(NSString* part in directory.pathComponents){current=[current URLByAppendingPathComponent:part];if([fm fileExistsAtPath:current.path])Regular(current,true);}
  NSURL* base=[root URLByAppendingPathComponent:directory];if(![fm fileExistsAtPath:base.path])continue;
  __block NSError* enumerationError=nil;NSDirectoryEnumerator* it=[fm enumeratorAtURL:base includingPropertiesForKeys:nil options:0 errorHandler:^BOOL(NSURL*,NSError* error){enumerationError=error;return NO;}];
  for(NSURL* url in it){NSDictionary* a=[fm attributesOfItemAtPath:url.path error:nil];if([a[NSFileType] isEqual:NSFileTypeDirectory])continue;Regular(url,false);
   NSArray* prefix=root.URLByResolvingSymlinksInPath.path.pathComponents;NSArray* parts=url.URLByResolvingSymlinksInPath.path.pathComponents;
   Require(parts.count>prefix.count&&[[parts subarrayWithRange:NSMakeRange(0,prefix.count)] isEqual:prefix],@"Data file escaped the selected folder.");
   NSString* relative=[[parts subarrayWithRange:NSMakeRange(prefix.count,parts.count-prefix.count)] componentsJoinedByString:@"/"];add(url,relative);
  }
  Require(!enumerationError,@"The data folder could not be enumerated.");
 }
 return hashes;
}
}
NSString* FolderName(){return [NSUserDefaults.standardUserDefaults stringForKey:Label]?:@"Not selected (local only)";}
NSString* Status(){return state;}
bool Enabled(){return [NSUserDefaults.standardUserDefaults dataForKey:Bookmark]!=nil;}
void Disconnect(){[NSUserDefaults.standardUserDefaults removeObjectForKey:Bookmark];[NSUserDefaults.standardUserDefaults removeObjectForKey:Label];state=@"Using local data. The Drive copy was retained.";}
Result Reconcile(NSURL* local,NSURL* folder,Resolution resolution){
 @try {try {
  NSFileManager* fm=NSFileManager.defaultManager;Regular(local,true);Regular(folder,true);
  local=local.URLByResolvingSymlinksInPath;folder=folder.URLByResolvingSymlinksInPath;
  NSString* lp=local.URLByStandardizingPath.path;NSString* fp=folder.URLByStandardizingPath.path;
  Require(![lp isEqual:fp]&&![lp hasPrefix:[fp stringByAppendingString:@"/"]]&&![fp hasPrefix:[lp stringByAppendingString:@"/"]],@"Choose a folder outside the app's working data.");
  NSDictionary* left=Scan(local);NSDictionary* right=Scan(folder);
  NSURL* metadata=[local URLByAppendingPathComponent:@".drive-sync" isDirectory:YES];
  if([fm fileExistsAtPath:metadata.path])Regular(metadata,true);else Require([fm createDirectoryAtURL:metadata withIntermediateDirectories:NO attributes:nil error:nil],@"Could not create local sync metadata.");
  NSString* key=Hash([fp dataUsingEncoding:NSUTF8StringEncoding]);NSURL* baseline=[metadata URLByAppendingPathComponent:[key stringByAppendingString:@".plist"]];
  if([fm fileExistsAtPath:baseline.path])Regular(baseline,false);
  NSDictionary* previous=[NSDictionary dictionaryWithContentsOfURL:baseline]?:@{};
  NSMutableSet* names=[NSMutableSet setWithArray:left.allKeys];[names addObjectsFromArray:right.allKeys];
  NSMutableArray* changes=[NSMutableArray array];NSMutableDictionary* merged=[NSMutableDictionary dictionary];
  for(NSString* name in [[names allObjects] sortedArrayUsingSelector:@selector(compare:)]){
   NSString* l=left[name];NSString* r=right[name];NSString* old=previous[name];bool toLocal=false;
   if(l&&r&&![l isEqual:r]){
    if(old&&[l isEqual:old])toLocal=true;
    else if(old&&[r isEqual:old])toLocal=false;
    else {if(resolution==Resolution::Ask)return {false,true,@"Both copies differ. Choose which version to keep for conflicting files."};toLocal=resolution==Resolution::Drive;}
   }else if(!l)toLocal=true;
   NSString* desired=toLocal?r:l;merged[name]=desired;
   if(l&&r&&[l isEqual:r])continue;
   [changes addObject:@{@"name":name,@"local":@(toLocal),@"digest":desired}];
  }
  // Stage every input before replacing anything. Memory stays bounded even for
  // ROMFS images and texture packs. No deletion propagation in either direction.
  NSURL* stage=[metadata URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];Require([fm createDirectoryAtURL:stage withIntermediateDirectories:NO attributes:nil error:nil],@"Could not stage data sync.");
  NSMutableArray* committed=[NSMutableArray array];
  try {
   NSUInteger index=0;
   for(NSDictionary* change in changes){NSURL* source=[( [change[@"local"] boolValue]?folder:local) URLByAppendingPathComponent:change[@"name"]];NSURL* copy=[stage URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu.new",(unsigned long)index++]];Require([fm copyItemAtURL:source toURL:copy error:nil],@"Could not download or stage a file. Local data is retained.");Require([Digest(copy) isEqual:change[@"digest"]],@"A file changed during sync. Try again.");}
   index=0;
   for(NSDictionary* change in changes){NSURL* target=[([change[@"local"] boolValue]?local:folder) URLByAppendingPathComponent:change[@"name"]];NSURL* parent=[target URLByDeletingLastPathComponent];Require([fm createDirectoryAtURL:parent withIntermediateDirectories:YES attributes:nil error:nil],@"Could not create a data directory.");NSURL* copy=[stage URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu.new",(unsigned long)index]];NSURL* backup=[stage URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu.old",(unsigned long)index++]];
    BOOL exists=[fm fileExistsAtPath:target.path];if(exists){Require([fm copyItemAtURL:target toURL:backup error:nil],@"Could not preserve the previous file.");}
    // NSData's atomic write would load large texture/ROM files into memory.
    // Copy to an adjacent temp and rename instead, including cross-volume sync.
    NSURL* adjacent=[parent URLByAppendingPathComponent:[@".remasked-sync-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    Require([fm copyItemAtURL:copy toURL:adjacent error:nil],@"Could not write the data file.");
    int result=rename(adjacent.fileSystemRepresentation,target.fileSystemRepresentation);if(result!=0){[fm removeItemAtURL:adjacent error:nil];throw std::runtime_error("Could not commit a data file");}
    [committed addObject:@{@"target":target,@"backup":backup,@"existed":@(exists)}];
   }
   Require([merged writeToURL:baseline atomically:YES],@"Could not save the sync baseline.");
  }catch(...){for(NSDictionary* entry in committed.reverseObjectEnumerator){NSURL* target=entry[@"target"];if([entry[@"existed"] boolValue]){[fm removeItemAtURL:target error:nil];[fm copyItemAtURL:entry[@"backup"] toURL:target error:nil];}else [fm removeItemAtURL:target error:nil];}throw;}
  [fm removeItemAtURL:stage error:nil];return {true,false,@"Drive folder is up to date. Gameplay uses local working files."};
 }catch(const std::exception& e){return {false,false,[NSString stringWithUTF8String:e.what()]};}}
 @catch(NSException* exception){return {false,false,@"The folder could not be synced. Local data is retained."};}
}
void Sync(NSURL* local,NSURL* selected,Resolution resolution,void (^completion)(Result)){
 if(busy){completion({false,false,@"Data sync is already running."});return;}busy=true;state=@"Syncing data folder…";
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool{
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
    result=Reconcile(local,data,resolution);
   }];
   if(error)result={false,false,@"The selected folder is unavailable. Local data is retained."};
   if(result.ok&&selected){NSData* saved=[selected bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:&error];if(saved){[NSUserDefaults.standardUserDefaults setObject:saved forKey:Bookmark];[NSUserDefaults.standardUserDefaults setObject:selected.lastPathComponent forKey:Label];}else result={false,false,@"Files synced, but folder access could not be remembered. Choose it again."};}
  }
  if(access)[url stopAccessingSecurityScopedResource];
  dispatch_async(dispatch_get_main_queue(),^{busy=false;state=result.message;completion(result);});
 }});
}
}
