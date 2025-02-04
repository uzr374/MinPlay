#ifndef CSUBTITLE_HPP
#define CSUBTITLE_HPP

#include <QtGlobal>

extern "C"{
#include <libavcodec/avcodec.h>
}

class CSubtitle final
{
private:
    AVSubtitle sub{};
    int sub_width = 0;
    int sub_height = 0;
    bool uploaded = false;
    double sub_pts = 0.0;
    double sub_duration = 0.0;
    double start_time = 0.0, end_time = 0.0;
    int serial = -1;

    void move_from(CSubtitle&& rhs);
    void copyParams(const CSubtitle& from);
public:
    CSubtitle();
    ~CSubtitle();
    CSubtitle(CSubtitle&& other);
    CSubtitle& operator=(CSubtitle&& other);
    CSubtitle(const CSubtitle& other);
    CSubtitle& operator=(const CSubtitle& other);

    void clear(){
        avsubtitle_free(&sub);
        sub = {};
        sub_width = sub_height = 0;
        sub_pts = sub_duration = start_time = end_time = 0.0;
        serial = -1;
        uploaded = false;
    }

    int width() const{ return sub_width;}
    int height() const{return sub_height;}
    int format() const {return sub.format;}
    bool isUploaded() const{return uploaded;}
    double ts() const{return sub_pts;}
    double duration() const{return sub_duration;}
    double startTime() const{return start_time;}
    double endTime() const{return end_time;}
    bool isBitmap() const{return sub.format == 0;}
    unsigned numRects() const{return sub.num_rects;}
    AVSubtitleRect** rects() const{return sub.rects;}
    const AVSubtitleRect& rectAt(int idx);
    int ser() const{return serial;}

    void setUploaded(bool upl){uploaded = upl;}
    void setSerial(int s) {serial = s;}
    void calcTimingInfo(){
        if(sub.pts != AV_NOPTS_VALUE){
            sub_pts = sub.pts/(double)AV_TIME_BASE;
        }
        start_time = sub_pts + sub.start_display_time / 1000.0;
        end_time = sub_pts + sub.end_display_time / 1000.0;
        sub_duration = end_time - start_time;
    }
    void ensureDimensions(int ref_w, int ref_h){
        if(!sub_width || !sub_height){
            sub_width = ref_w;
            sub_height = ref_h;
        }
    }

    const AVSubtitle& constAv() const{return sub;}
    AVSubtitle& av(){return sub;}
};

#endif // CSUBTITLE_HPP
