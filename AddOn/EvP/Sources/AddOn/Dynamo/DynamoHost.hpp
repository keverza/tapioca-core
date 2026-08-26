#ifndef EVP_DYNAMOHOST_HPP
#define EVP_DYNAMOHOST_HPP

namespace evp::dynamo {

// Opens the pinned Dynamo 4 editor in its own .NET 10 process. Dynamo 4 cannot
// share Archicad's process-wide .NET 8 runtime with Rhino.Inside.
void OpenFromMenu ();

// Drops native process handles during add-on teardown. The standalone editor
// remains user-owned and does not retain callbacks into the APX.
void Release ();

} // namespace evp::dynamo

#endif
