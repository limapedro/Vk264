#include <stdio.h>
#include "encoder.h"

int main(void)
{
    vk_encoder_t e; vk_encoder_init(&e,30,3,1);
    /* Simulate a GOP: I(0), B(1), B(2), P(3), P(4) in display order,
     * coding order 0,3,1,2,4. */
    const int display[5]={0,3,1,2,4};
    const vk_frame_type_t types[5]={VK_FRAME_IDR,VK_FRAME_P,VK_FRAME_B,VK_FRAME_B,VK_FRAME_P};
    const int fn[5]={0,1,1,1,2};
    const int poc[5]={0,6,2,4,8};
    vk_frame_t st[5];
    for(int i=0;i<5;++i){
        st[display[i]].i_frame=display[i]; st[display[i]].type=types[i];
        st[display[i]].frame_num=fn[i]; st[display[i]].poc=poc[i];
    }
    for(int c=0;c<5;++c){
        vk_enc_frame_state_t fs;
        vk_encoder_frame_begin(&e,display[c],&st[display[c]],&fs);
        printf("c=%d fn=%d poc=%d idr=%d ref=%d key=%d\n",c,
               fs.i_frame_num,fs.i_poc,fs.i_idr_pic_id,fs.nal_ref_idc,fs.b_keyframe);
        if(fs.i_frame_num!=fn[c]||fs.i_poc!=poc[c]){
            fprintf(stderr,"FAIL frame state at %d\n",c);return 1;
        }
        vk_encoder_frame_end(&e,&fs);
    }
    printf("encoder orchestration fixture PASS\n");
    return 0;
}
