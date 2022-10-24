#include <linux/types.h>
#include "api_version.h"

bool api_with_seid = false;
bool api_with_urr_bar = false;
bool api_far_action_u16 = false;

void set_api_with_seid(bool val){
    api_with_seid = val;
}

bool get_api_with_seid(){
    return api_with_seid;
}

void set_api_with_urr_bar(bool val){
    api_with_urr_bar = val;
}

bool get_api_with_urr_bar(){
    return api_with_urr_bar;
}

void set_far_action_u16(bool val){
    api_far_action_u16 = val;
}

bool far_action_is_u16(){
    return api_far_action_u16;
}