#ifndef EVP_ARCHVIZ_VIEWERFRAMEPACING_HPP
#define EVP_ARCHVIZ_VIEWERFRAMEPACING_HPP

// The standalone viewer's frame pacing, as a diagnostic line in archviz.log.
//
// The viewer reported only its frame COUNT, and a smooth 144 fps and a 144 fps
// that drops one frame in ten average to the same number. Navigation that
// "became jittery" is the second case or it is not; this tells them apart.
//
// Fed the time of each Present; every window it returns one line (empty
// otherwise): the median interval, the 99th percentile, the worst, and how
// many intervals ran past twice the median -- the hitches a person sees.

#include <string>
#include <vector>

namespace geomsrv::archviz {

class ViewerFramePacing {
  public:
    explicit ViewerFramePacing (double windowSeconds = 10.0) : window_ (windowSeconds) {}

    std::string Note (double nowSeconds);

  private:
    double window_;
    double started_ = -1.0;
    double last_ = -1.0;
    std::vector<double> intervals_; // milliseconds
};

} // namespace geomsrv::archviz

#endif
