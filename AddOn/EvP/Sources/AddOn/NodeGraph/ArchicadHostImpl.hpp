#ifndef EVP_NODEGRAPH_ARCHICADHOSTIMPL_HPP
#define EVP_NODEGRAPH_ARCHICADHOSTIMPL_HPP

// The ACAPI implementation of IArchicadHost. Declared here so add-on startup can
// install it; nothing in the graph runtime names this type.

#include "NodeGraph/ArchicadHost.hpp"

namespace evp::nodegraph {

class ArchicadGenerationSource final : public IProjectGenerationSource {
  public:
    bool Sample (GenerationDomain domain, uint64_t& value, std::string& error) const override;
};

class ArchicadReferenceResolver final : public IReferenceResolver {
  public:
    ReferenceResolution Resolve (const Reference& reference) const override;
    std::vector<ReferenceResolution> ResolveAll (const std::vector<Reference>& references) const override;
};

class ArchicadHostImpl final : public IArchicadHost {
  public:
    static ArchicadHostImpl& Get ();

    bool IsAvailable () const override;
    const IProjectGenerationSource& Generations () const override;
    const IReferenceResolver& References () const override;
    bool GetSelection (std::vector<ArchicadElementRef>& elements, std::string& error) const override;
    bool SetSelection (const std::vector<ArchicadElementRef>& elements, std::string& error) override;

  private:
    ArchicadGenerationSource generations_;
    ArchicadReferenceResolver references_;
};

} // namespace evp::nodegraph

#endif
