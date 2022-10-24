#ifndef __API_VERSION_H__
#define __API_VERSION_H__

extern void set_api_with_seid(bool);
extern bool get_api_with_seid(void);

extern void set_api_with_urr_bar(bool);
extern bool get_api_with_urr_bar(void);

extern void set_far_action_u16(bool);
extern bool far_action_is_u16(void);

#endif // __API_VERSION_H__