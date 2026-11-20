// SPDX-License-Identifier: GPL-2.0-or-later
#include "fast/extension/remasked_duo_layout.h"
#include <cassert>
#include <limits>
using namespace ReMaskedDuo;
int main(){
 assert(!HingeCapability(true,true,0,false)); // ordinary iPhone's placeholder
 assert(HingeCapability(true,true,1,false)); // closed Duo
 assert(HingeCapability(true,true,2,false));
 assert(HingeCapability(true,true,3,false));
 assert(HingeCapability(true,true,0,true)); // inactive flat divider
 assert(HingeCapability(true,true,0,false,true)); // transient unknown
 assert(!HingeCapability(true,false,0,false,true)); // detached hierarchy
 assert(!HingeCapability(false,true,3,true));
 Display d;ReMaskedDual::Requested=true;
 assert(!d.Pixels(900,700,true).Dual());
 d.Publish({true,false,{10,20,900,700},{20,40,880,660},{440,20,40,700},{}});
 auto a=d.Pixels(900,700,true),retina=d.Pixels(2700,2100,true);
 assert(a.Dual()&&a.Top.X+a.Top.Width<=430&&a.Lower.X>=470);
 assert(retina.Lower.X==3*a.Lower.X);
 auto t=d.MapTouch(a.Lower.X+10+a.Lower.Width/2,a.Lower.Y+20+a.Lower.Height/2,true);
 assert(t&&t->X==160&&t->Y==120);
 assert(!d.MapTouch(450,350,true));
 d.Publish({true,false,{0,0,900,700},{0,0,900,700},{450,0,0,700},{}});
 assert(d.Pixels(900,700,true).Dual()); // UIKit's inactive divider has zero width when flat.
 assert(!d.MapTouch(700,350,false));
 d.Publish({true,false,{0,0,700,900},{0,20,700,860},{0,420,700,40},{{300,500,100,100}}});
 a=d.Pixels(700,900,true);assert(a.Dual()&&a.Lower.Y>=460);
 assert(!d.MapTouch(350,550,true));
 d.Publish({true,true,{0,0,400,700},{0,20,400,660},{},{}});
 assert(!d.Pixels(400,700,true).Dual()&&!ReMaskedDual::Available.load());
 assert(ReMaskedDual::Requested.load());
 d.Publish({false,false,{0,0,900,700},{0,0,900,700},{440,0,20,700},{}});
 assert(!d.Pixels(900,700,true).Dual());
 d.Publish({true,false,{0,0,900,700},{0,0,900,700},{440,100,20,400},{}});
 assert(!d.Pixels(900,700,true).Dual()); // a camera-sized occlusion is not a divider
 assert(!d.Pixels(std::numeric_limits<float>::quiet_NaN(),700,true).Dual());
}
