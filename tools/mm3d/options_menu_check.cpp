// clang++ -std=c++20 -Isrc tools/mm3d/options_menu_check.cpp -o /tmp/options-menu-check
#define main native_inventory_checks_main
#include "native_inventory_check.cpp"
#undef main
#include "mm3d/options_menu.h"
namespace O=MM3D::OptionsMenu;
void OptionsChecks(const O::Options& options) {
    O::Apply(options);
    Memory m;O::Hook hook;
    auto manager=m.Alloc(32),from=m.Alloc(0x848),to=m.Alloc(0x848),back=m.Alloc(0x848);
    m.words[manager+28]=from;m.words[manager+20]=to;m.words[manager+24]=back;
    auto owner=m.Alloc(0x1b8),panel=m.Layout("MenuOption"),cursor=m.Layout("CursorOption",8,4),bg=m.Layout("CommonBottomBg"),other=m.Layout("Unrelated");
    m.words[owner+0x10]=panel;m.words[owner+0x14]=bg;m.words[owner+0x18]=cursor;
    auto all=m.Widget("all"),page1=m.Widget("page1_g"),page2=m.Widget("page2_g",320,0);
    auto quit=m.Widget("btn_return_l"),p1=m.Widget("btn_page1_g"),p2=m.Widget("btn_page2_g"),gray=m.Widget("grayout_freecam_g");
    m.Children(panel+8,{all});m.Children(all,{page1,page2,quit,p1,p2});
    std::vector<U32> first,second;std::array<U32,8> rows,labels;
    for(unsigned i=0;i<8;++i) {
        const float x=(i/4)*320+8,y=4+48*(i%4);
        auto row=m.Widget(O::Rows[i],x,y),embedded=m.Layout("BasicPlate",x,y),label=m.Widget("label",x+6,y+10);
        rows[i]=row;labels[i]=label;m.words[row+0x20]=embedded;m.Children(embedded+8,{label});
        m.words[label+0x134]=std::bit_cast<U32>(i==2?.5f:1.f);
        if(i==2){m.Children(gray,{row});first.push_back(gray);}else(i<4?first:second).push_back(row);
    }
    m.Children(page1,first);m.Children(page2,second);
    m.Queue(from,{panel,cursor,other});m.Queue(back,{bg,other});m.Queue(to,{other});
    hook.Capture(m,owner);assert(hook.owner==owner);hook.Pass(m,manager,4);
    assert(hook.panel==panel&&m.words[from+0x844]==1&&m.words[from+4+0x80]==other);
    assert(m.words[to+0x844]==3&&m.words[back+0x844]==1&&m.words[back+4+0x80]==other);
    hook.Pass(m,manager,3);
    for(unsigned selected=0;selected<8;++selected) {
        m.words[owner+8]=2|(selected<<16);auto before=m.words;
        for(unsigned requested=0;requested<8;++requested)
            assert(O::Selection(m,owner,requested)==(requested==3?(selected>3?2u:4u):requested));
        hook.Begin(m,panel);
        for(unsigned i=0;i<8;++i) {
            if(i==3){assert(!(m.words[rows[i]+0xd4]&1));continue;}
            auto target=O::Position(i,options);const auto f=[&](U32 at){return std::bit_cast<float>(m.words[at]);};
            assert(std::abs(f(rows[i]+0xec)-target[0])<.0001f&&std::abs(f(rows[i]+0xfc)-target[1])<.0001f);
            assert(std::abs(f(labels[i]+0xec)-(target[0]+6*options.scale))<.0001f);
            assert(f(labels[i]+0x134)==(i==2?.5f:1.f));
        }
        assert(!(m.words[quit+0xd4]&1)&&!(m.words[p1+0xd4]&1)&&!(m.words[p2+0xd4]&1));
        auto adjusted=m.words;hook.Begin(m,other);assert(m.words==adjusted);hook.End(m);hook.End(m);assert(m.words==before);
        hook.Begin(m,cursor);auto target=O::Position(selected,options);
        if(selected==3)assert(!(m.words[cursor+8+0xd4]&1));
        else {
            assert(std::abs(std::bit_cast<float>(m.words[cursor+8+0xec])-target[0])<.0001f);
            assert(std::abs(std::bit_cast<float>(m.words[cursor+8+0xfc])-target[1])<.0001f);
        }
        hook.End(m);assert(m.words==before);
    }
    auto message=m.Layout("MessageWindow",0,0,0x480),composition=m.Widget("all"),plate=m.Widget("plate"),text=m.Widget("text"),front=m.Widget("front");
    m.Children(message+8,{composition});m.Children(composition,{plate,text,front});m.words[composition+8]=message+8;
    const auto bits=[](float v){return std::bit_cast<U32>(v);};
    m.words[composition+0x98]=m.words[composition+0x9c]=m.words[plate+0x98]=bits(1);
    m.words[composition+0x8c]=bits(200);m.words[composition+0x90]=bits(180);
    m.words[message+0x3f4]=0x5745;assert(hook.IsDescription(m,message));
    const auto description_before=m.words;MM3D::DialogueScale::Hook description;
    description.Begin(m,message,false,false,&hook.frame.description);
    assert(m.words[composition+0x98]==bits(options.description.scale));
    assert(m.words[composition+0x8c]==bits(200+options.description.x));
    assert(m.words[composition+0x90]==bits(180+options.description.y));
    description.End(m,message);
    // The existing dialogue hook retains its dirty flags so native transforms
    // are recalculated; all pose values and unrelated memory are restored.
    for(auto [at,value]:description_before)
        assert(m.words.at(at)==(at==composition+0xd4?(value|0x1b00u):value));
    m.words[message+0x3f4]=0x1700;assert(!hook.IsDescription(m,message));
    m.words[message+0x3f4]=0x5751;m.words[owner+8]|=0x300;assert(!hook.IsDescription(m,message));
    m.words[owner+8]&=~0xff00u;assert(hook.IsDescription(m,message));
    hook.Pass(m,manager,5);auto before=m.words;hook.Begin(m,panel);assert(m.words==before);hook.End(m);
    // Missing rows must not remove anything from the native queues.
    m.Queue(from,{panel,cursor,other});m.Queue(back,{bg,other});m.Queue(to,{other});
    m.words[rows[7]+0x10]=m.String("wrong_row");before=m.words;
    hook.Pass(m,manager,4);assert(hook.panel==0&&m.words==before);assert(!hook.IsDescription(m,message));
}
int main(int argc,char** argv) {
    assert(argc==2);Memory code;std::ifstream input(argv[1],std::ios::binary);assert(input);
    U32 word;for(U32 at=0x100000;input.read(reinterpret_cast<char*>(&word),4);at+=4)code.words[at]=word;
    assert(O::CodeMatches(code));unsigned changed=0;
    for(auto [at,value]:code.words)changed+=O::Instruction(at,value)!=value;
    assert(changed==5);
    const auto skip=O::Instruction(O::CalibrationPC,code.words[O::CalibrationPC]);
    assert(skip==0xea000014); // unconditional branch past every calibration side effect
    assert(O::CalibrationPC+8+((skip&0xffffff)<<2)==0x5a2d44);
    assert(O::Instruction(O::PagePC,code.words[O::PagePC])==0xe3a00000);
    assert(O::Instruction(O::PagePC+4,code.words[O::PagePC+4])==0xe12fff1e);
    code.words[O::DrawPC]^=1;assert(!O::CodeMatches(code));
    OptionsChecks({});
    OptionsChecks({12,25,.6f,44,{-50,20,.75f}});
    OptionsChecks({-20,8,1.2f,60,{30,-45,1.5f}});
    O::Apply({});std::cout<<"Options menu checks passed\n";
}
