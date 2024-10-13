#ifndef CSUBTITLE_HPP
#define CSUBTITLE_HPP

extern "C"{
#include <libavcodec/avcodec.h>
}

class CSubtitle
{
    AVSubtitle sub{};
    double s_pts = 0.0, s_duration = 0.0;

public:
    CSubtitle();
};

struct Subtitle final {
private:
    AVSubtitle sub{};
    int sub_width = 0;
    int sub_height = 0;
    bool uploaded = false;
    double sub_pts = 0.0;
    double sub_duration = 0.0;

public:
    void clear(){
        avsubtitle_free(&sub);
        sub_width = sub_height = 0;
        sub_pts = sub_duration = 0.0;
        uploaded = false;
    }
    void unref(){ //For compatibility
        clear();
    }

private:
    void move_from(Subtitle&& rhs){
        clear();
        sub = rhs.sub;
        rhs.sub = {};
        sub_width = rhs.sub_width;
        sub_height = rhs.sub_height;
        uploaded = false;
        sub_pts = rhs.sub_pts;
        sub_duration = rhs.sub_duration;
        rhs.clear();
    }

public:
    Subtitle(){}
    Subtitle(AVSubtitle& src_sub, int w, int h, double pts, double dur){
        sub = src_sub;
        sub_width = w;
        sub_height = h;
        sub_pts = pts;
        sub_duration = dur;
    }
    ~Subtitle(){avsubtitle_free(&sub);}
    Subtitle(Subtitle&& rhs){
        move_from(std::move(rhs));
    }
    //Q_DISABLE_COPY(Subtitle);
    Subtitle& operator=(Subtitle&& rhs) {
        move_from(std::move(rhs));
        return *this;
    }

    int width() const{
        return sub_width;
    }

    int height() const{return sub_height;}
    int format() const {return sub.format;}
    bool isUploaded() const{return uploaded;}
    double ts() const{return sub_pts;}
    double duration() const{return sub_duration;}
    void setUploaded(bool upl){uploaded = upl;}

    void ensureDimensions(int ref_w, int ref_h){
        if(!sub_width || !sub_height){
            sub_width = ref_w;
            sub_height = ref_h;
        }
    }

    void setParams(){}

    const AVSubtitle& constAv() const{return sub;}
    AVSubtitle& av(){return sub;}
};

#endif // CSUBTITLE_HPP
