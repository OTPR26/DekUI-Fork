// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>
#include "../runtime/ios/drive_data.h"
#include <cstdio>
#include <cstdlib>
namespace {
void Check(bool pass,const char* message){if(!pass){fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
void Put(NSURL* root,NSString* relative,NSString* text){NSURL* url=[root URLByAppendingPathComponent:relative];Check([NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil],"mkdir fixture");Check([[text dataUsingEncoding:NSUTF8StringEncoding] writeToURL:url atomically:YES],"write fixture");}
NSString* Get(NSURL* root,NSString* relative){return [NSString stringWithContentsOfURL:[root URLByAppendingPathComponent:relative] encoding:NSUTF8StringEncoding error:nil];}
}
int main(){@autoreleasepool{
 NSFileManager* fm=NSFileManager.defaultManager;NSURL* base=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];NSURL* local=[base URLByAppendingPathComponent:@"local"];NSURL* drive=[base URLByAppendingPathComponent:@"drive"];
 Check([fm createDirectoryAtURL:local withIntermediateDirectories:YES attributes:nil error:nil],"create local");Check([fm createDirectoryAtURL:drive withIntermediateDirectories:YES attributes:nil error:nil],"create drive");
 using namespace ReMaskedDrive;
 Put(local,@"models/test/import.json",@"{\"source_form\":0}");Put(local,@"presentation.json",@"{\"adult_link_model\":true}");Put(local,@"textures/load/test.txt",@"synthetic texture fixture");Put(local,@"cache/pipeline.bin",@"device-specific cache");Put(local,@"logs/private.txt",@"private diagnostic fixture");Put(local,@"ReMaskedNativeGame.launch.json",@"device-local paths");
 auto initial=Reconcile(local,drive,Resolution::Ask);if(!initial.ok)fprintf(stderr,"Initial sync: %s\n",initial.message.UTF8String);Check(initial.ok,"initial upload");Check([Get(drive,@"models/test/import.json") isEqual:Get(local,@"models/test/import.json")],"model copied");Check(!Get(drive,@"cache/pipeline.bin")&&!Get(drive,@"logs/private.txt")&&!Get(drive,@"ReMaskedNativeGame.launch.json"),"caches logs and launch paths remain local");
 Check(Reconcile(local,drive,Resolution::Ask).ok,"idempotent sync");Put(drive,@"presentation.json",@"{\"adult_link_model\":false}");Check(Reconcile(local,drive,Resolution::Ask).ok,"remote-only change");Check([Get(local,@"presentation.json") isEqual:Get(drive,@"presentation.json")],"remote imported");
 Put(local,@"presentation.json",@"{\"adult_link_model\":true}");Check(Reconcile(local,drive,Resolution::Ask).ok,"local-only change");
 Put(local,@"presentation.json",@"{\"adult_link_model\":true,\"x\":1}");Put(drive,@"presentation.json",@"{\"adult_link_model\":false,\"x\":2}");Put(local,@"models/test/new.txt",@"pending upload");
 auto conflict=Reconcile(local,drive,Resolution::Ask);Check(!conflict.ok&&conflict.conflict,"detect divergent copies");Check(!Get(drive,@"models/test/new.txt"),"conflict is detected before any writes");Check([Get(local,@"presentation.json") containsString:@"true"],"conflict preserves local");Check(Reconcile(local,drive,Resolution::Drive).ok,"explicit Drive conflict resolution");Check([Get(local,@"presentation.json") containsString:@"false"],"Drive chosen");
 Put(local,@"presentation.json",@"local choice");Put(drive,@"presentation.json",@"remote choice");Check(Reconcile(local,drive,Resolution::Local).ok,"explicit local conflict resolution");Check([Get(drive,@"presentation.json") isEqual:@"local choice"],"local chosen");
 Check(!Reconcile(local,[base URLByAppendingPathComponent:@"missing"],Resolution::Ask).ok,"offline folder does not destroy working data");Check([Get(local,@"presentation.json") isEqual:@"local choice"],"offline local intact");
 Check(!Reconcile(local,local,Resolution::Ask).ok,"reject same directory");NSURL* nested=[local URLByAppendingPathComponent:@"nested"];[fm createDirectoryAtURL:nested withIntermediateDirectories:NO attributes:nil error:nil];Check(!Reconcile(local,nested,Resolution::Ask).ok,"reject nested root");
 NSURL* link=[drive URLByAppendingPathComponent:@"models/link"];Check([fm createSymbolicLinkAtURL:link withDestinationURL:base error:nil],"make symlink");Check(!Reconcile(local,drive,Resolution::Ask).ok,"reject symlink escape");[fm removeItemAtURL:link error:nil];
 Put(drive,@"saves/save0.bin",@"bad size");Check(!Reconcile(local,drive,Resolution::Ask).ok,"reject invalid save before modifying data");[fm removeItemAtURL:[drive URLByAppendingPathComponent:@"saves/save0.bin"] error:nil];
 NSMutableData* save=[NSMutableData dataWithLength:79384];NSURL* saveURL=[local URLByAppendingPathComponent:@"saves/save0.bin"];[fm createDirectoryAtURL:saveURL.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];Check([save writeToURL:saveURL atomically:YES],"write synthetic save");Check(Reconcile(local,drive,Resolution::Ask).ok,"valid native save copied");Check([[NSData dataWithContentsOfURL:[drive URLByAppendingPathComponent:@"saves/save0.bin"]] isEqual:save],"save content preserved");
 // Deleting one side never deletes the remaining working copy.
 [fm removeItemAtURL:[drive URLByAppendingPathComponent:@"models/test/new.txt"] error:nil];Check(Reconcile(local,drive,Resolution::Ask).ok,"deletion does not propagate");Check(Get(drive,@"models/test/new.txt")!=nil,"missing copy restored");
 [fm removeItemAtURL:base error:nil];puts("Drive data transaction checks passed");return 0;
}}
