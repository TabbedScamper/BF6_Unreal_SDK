#include "bf6_core.h"
#include <cstdio>
#include <vector>
#include <cstring>
#include <cmath>
#include <DirectXMath.h>
using namespace DirectX;
static XMMATRIX matrix(const float* m) {return XMMATRIX(m[0],m[1],m[2],0,m[3],m[4],m[5],0,m[6],m[7],m[8],0,m[9],m[10],m[11],1);}
int main() {
 char err[1024]{};
 auto* c=bf6_open("C:/Program Files (x86)/Steam/steamapps/common/Battlefield 6",err,1024);
 if(!c)return 1;
 bf6_mount_frontend(c,err,1024);
 const char* clips[]={
 "animations/glacier/assets/frontend/mainmenu/loadout/ui_frontend_standing_idle_assault_01",
 "animations/glacier/assets/3p/common/rifle/loco/p_3p_rifle_stand_idle_01",
 "animations/glacier/assets/3p/common/rifle/loco/l_3p_rifle_stand_idle_01",
 "animations/glacier/assets/3p/common/rifle/loco/l_3p_rifle_stand_idle_lowintensity_01",
 "animations/glacier/assets/frontend/mainmenu/loadout/ui_frontend_standing_idle_cl_assault_02"
 };
 bf6_asset mdRows[128]{};int mdCount=bf6_list_ebx(c,"/md_m4a1",mdRows,128);const char* md=nullptr;
 for(int i=0;i<mdCount&&i<128;++i){std::printf("MD %s\n",mdRows[i].name);if(std::strstr(mdRows[i].name,"/md_m4a1")&& !std::strstr(mdRows[i].name,"bundle"))md=mdRows[i].name;}
 for(auto* name:{"Wep_Align","Wep_Root","Wep_IK_LeftHand","Wep_IK_RightHand"}) {
  bf6_bone_xform m{}; int n=bf6_weapon_bone_transform(c,md,name,&m);
  std::printf("WEAPON %s %d:",name,n);for(float x:m.m)std::printf(" %.4f",x);std::printf("\n");
 }
 for(auto* clip:clips){
  bf6_anim_binding_stats st{};
  int n=bf6_anim_bindings(c,clip,"animations/glacier/global/rigging/soldier_3p.rig","common/characters/_soldier/ske_soldier_3p",nullptr,0,&st);
  bf6_anim_clip* a=bf6_anim_clip_open(c,clip);
  std::printf("%s\n bindings=%d bones=%d q=%d v=%d clip=%s",clip,n,st.resolved_bones,st.quaternion_bones,st.vector_bones,a?"yes":"no");
  if(a){std::vector<float> samples(a->channel_count*4);int sample=bf6_anim_clip_sample(c,a,0,samples.data(),nullptr);std::printf(" channels=%d frames=%d sample=%d\n",a->channel_count,a->key_time_count,sample);
   if(n>0){
    std::vector<bf6_anim_binding> b(n);bf6_anim_bindings(c,clip,"animations/glacier/global/rigging/soldier_3p.rig","common/characters/_soldier/ske_soldier_3p",b.data(),n,&st);
    auto* sk=bf6_skeleton_compose(c,"common/characters/_soldier/ske_soldier_3p",nullptr);
    std::vector<XMFLOAT4X4> local(sk->bone_count),model(sk->bone_count);
    for(int i=0;i<sk->bone_count;i++)XMStoreFloat4x4(&local[i],matrix(sk->bones[i].local));
    for(auto& k:b){if(k.bone<0)continue;auto* v=samples.data()+k.channel*4;
     if(k.component==0){XMFLOAT4X4 q;XMStoreFloat4x4(&q,XMMatrixRotationQuaternion(XMQuaternionNormalize(XMVectorSet(v[0],v[1],v[2],v[3]))));for(int r=0;r<3;r++)for(int c=0;c<3;c++)local[k.bone].m[r][c]=q.m[r][c];}
     if(k.component==1)for(int c=0;c<3;c++)local[k.bone].m[3][c]=v[c];
    }
    for(int i=0;i<sk->bone_count;i++){auto m=XMLoadFloat4x4(&local[i]);if(sk->bones[i].parent>=0)m=XMMatrixMultiply(m,XMLoadFloat4x4(&model[sk->bones[i].parent]));XMStoreFloat4x4(&model[i],m);
     auto* name=sk->bones[i].name;if(name&&(std::strstr(name,"Wep_")||std::strcmp(name,"RightHand")==0||std::strcmp(name,"LeftHand")==0))std::printf(" SOLDIER %s model=(%.4f %.4f %.4f) local=(%.4f %.4f %.4f)\n",name,model[i]._41,model[i]._42,model[i]._43,local[i]._41,local[i]._42,local[i]._43);
    }
    bf6_free(c,sk);
   }
   bf6_free(c,a);}
  std::printf("\n");
 }
 bf6_close(c);
}
