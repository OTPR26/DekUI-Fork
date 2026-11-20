// SPDX-License-Identifier: GPL-2.0-or-later
#import "../runtime/save_sync.mm"
#include <cassert>
int main() { @autoreleasepool {
 NSFileManager* fm=NSFileManager.defaultManager;
 NSString* temp=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 root=[temp stringByAppendingPathComponent:@"saves"];
 assert([fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil]);
 for(NSString* name in Names()) {
  NSMutableData* data=[NSMutableData dataWithLength:79384];
  memset(data.mutableBytes,0x41,data.length);
  assert([data writeToFile:[root stringByAppendingPathComponent:name] atomically:YES]);
 }
 NSData* original=Snapshot(root);assert(original && Validate(original).count==3);
 assert([original isEqual:Snapshot(root)]);
 NSMutableDictionary* corrupt=[[NSJSONSerialization JSONObjectWithData:original options:NSJSONReadingMutableContainers error:nil] mutableCopy];
 corrupt[@"files"][@"../outside"] = @"";
 assert(!Validate([NSJSONSerialization dataWithJSONObject:corrupt options:0 error:nil]));
 assert(!Validate([@"{}" dataUsingEncoding:NSUTF8StringEncoding]));
 assert(!Restore([@"{}" dataUsingEncoding:NSUTF8StringEncoding]));
 assert([original isEqual:Snapshot(root)]);
 NSString* path=[root stringByAppendingPathComponent:@"save0.bin"];
 NSMutableData* changed=[NSMutableData dataWithLength:79384];memset(changed.mutableBytes,0x42,changed.length);
 assert([changed writeToFile:path atomically:YES]);
 NSData* changedSnapshot=Snapshot(root);assert(![changedSnapshot isEqual:original]);
 assert(Restore(original));assert([original isEqual:Snapshot(root)]);
 bool backupFound=false;
 for(NSString* name in [fm contentsOfDirectoryAtPath:temp error:nil])
  if([name hasPrefix:@"save-backup-"])backupFound=[changedSnapshot isEqual:Snapshot([temp stringByAppendingPathComponent:name])];
 assert(backupFound);
 [fm removeItemAtPath:path error:nil];
 assert([fm createSymbolicLinkAtPath:path withDestinationPath:@"/dev/null" error:nil]);
 assert(!Snapshot(root));assert(!Restore(original));
 [fm removeItemAtPath:temp error:nil];
 }}
