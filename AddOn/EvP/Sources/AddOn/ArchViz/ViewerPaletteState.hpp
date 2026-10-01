#ifndef EVP_ARCHVIZ_VIEWERPALETTESTATE_HPP
#define EVP_ARCHVIZ_VIEWERPALETTESTATE_HPP

namespace geomsrv::archviz {

// Main-thread policy, independent of DG: a retained hidden palette is not an
// open viewer. Only a balanced host hide of a user-open palette may restore it.
class ViewerPaletteState final {
  public:
    void RequestOpen ()
    {
        requestedOpen_ = true;
        if (hostHideDepth_ > 0)
            restorePending_ = true;
    }

    void RequestClose ()
    {
        requestedOpen_ = false;
        restorePending_ = false;
    }

    bool CanShow () const
    {
        return requestedOpen_ && hostHideDepth_ == 0;
    }

    void BeginHostHide (bool visible)
    {
        if (hostHideDepth_++ == 0)
            restorePending_ = requestedOpen_ && visible;
    }

    bool EndHostHide ()
    {
        if (hostHideDepth_ == 0 || --hostHideDepth_ > 0)
            return false;
        const bool restore = requestedOpen_ && restorePending_;
        restorePending_ = false;
        return restore;
    }

  private:
    bool requestedOpen_ = false;
    bool restorePending_ = false;
    unsigned hostHideDepth_ = 0;
};

} // namespace geomsrv::archviz

#endif
