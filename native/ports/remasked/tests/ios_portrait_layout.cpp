// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/portrait_layout.h"
#include "fast/extension/presentation_picture.h"
#include <cassert>
#include <limits>
int main(){
 using namespace ReMaskedIOS;
 auto a=ResolvePortraitLayout({0,0,440,956},{0,62,440,860},true);
 assert(a.Active() && a.Game.Y==78 && a.Game.X==0);
 assert(a.Game.Y+a.Game.Height+20<=a.Controls.Y);
 assert(a.Controls.Y+a.Controls.Height==922);
 assert(a.Game.Width/a.Game.Height>1.777f && a.Game.Width/a.Game.Height<1.778f);
 assert(!ResolvePortraitLayout({0,0,956,440},{62,0,860,406},true).Active());
 assert(!ResolvePortraitLayout({0,0,440,956},{0,62,440,860},false).Active());
 assert(!ResolvePortraitLayout({0,0,300,360},{0,40,300,280},true).Active());
 assert(!ResolvePortraitLayout({0,0,440,956},{0,62,440,860},true,std::numeric_limits<float>::quiet_NaN()).Active());
 PhonePortrait.Publish({0,0,440,956},{0,62,440,860},true);
 auto pixels=PhonePortrait.Pixels(1320,2868);
 assert(pixels.Active() && pixels.Game.X==0 && pixels.Game.Y==234);
 assert(pixels.Controls.Y+pixels.Controls.Height==2766);
 auto picture=Fast::Extension::ResolvePresentationPicture(1320,2868,Fast::Extension::AspectRatioMode::Wide16x9);
 assert(picture.X==0 && picture.Y==234 && picture.Width==1320 && picture.Height==743);
 PhonePortrait.Publish({0,0,956,440},{62,0,860,406},true);
 auto landscape=Fast::Extension::ResolvePresentationPicture(2868,1320,Fast::Extension::AspectRatioMode::Wide16x9);
 assert(landscape.Y==0 && landscape.X>0 && landscape.Height==1320);
 PhonePortrait.Publish({0,0,440,956},{0,62,440,860},false);
 assert(!PhonePortrait.Pixels(1320,2868).Active());

 auto rows=ResolvePortraitControlRows(a.Controls.Height,1.f);
 assert(a.Game.Width==440 && a.Controls.Y==a.Game.Y+a.Game.Height+20);
 assert(rows.ThumbCenter+66+32<=rows.SystemTop);
 assert(rows.ThumbCenter-66>95); // clears the two shoulder rows
 assert(rows.SystemTop+30<=a.Controls.Height-16);

}
