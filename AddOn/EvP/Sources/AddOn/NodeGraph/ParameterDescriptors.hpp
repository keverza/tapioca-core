#ifndef EVP_NODEGRAPH_PARAMETERDESCRIPTORS_HPP
#define EVP_NODEGRAPH_PARAMETERDESCRIPTORS_HPP

// The parameter UI descriptors the node families share.
//
// ⚠️ ONE HOME, BECAUSE A "LENGTH IN METRES" HAS TO LOOK THE SAME EVERYWHERE. A
// Box's width and an Arc's radius are the same kind of thing, and a copy of the
// builder in each family would drift on the day one of them gained a unit, a
// step or a decimal place - at which point two fields that mean the same thing
// behave differently and nothing says why.
//
// These describe how a value is EDITED and nothing else. The node still clamps
// and validates its own inputs: a control that ignored a range would produce a
// wrong-looking box, never a wrong answer. See NodeType.hpp on ParameterWidget.

#include "NodeGraph/NodeType.hpp"

namespace evp::nodegraph {

// A number typed into a field. `decimalsFrom` names a SIBLING parameter that
// governs the precision, for the nodes where the user controls it.
ParameterUi NumberUi (const char* section, int order, const char* help, int decimals,
                      const char* decimalsFrom = nullptr);

// A length in metres, typed rather than dragged: a wall is 0.3 and a site is
// 200, so there is no range a slider could span without being useless at one end.
ParameterUi LengthUi (const char* section, int order, const char* help);

// A position, as one row of three fields.
ParameterUi PointUi (const char* section, int order, const char* help);

// A direction, likewise. Distinct from PointUi only in the widget, which is what
// tells a client it may offer a pick-in-model affordance for one and not the
// other.
ParameterUi VectorUi (const char* section, int order, const char* help);

// ⚠️ ANGLES ARE AUTHORED IN DEGREES AND COMPUTED IN RADIANS. Nobody types
// 1.5707963 for a right angle. The unit says which one the field is, and every
// angle-taking node converts on the way in - so a value that reads 90 anywhere
// it is shown really is a quarter turn.
ParameterUi AngleUi (const char* section, int order, const char* help);

// A whole number with a floor and a ceiling. The ceiling is the point: every one
// of these drives an allocation, and a typed extra zero should be refused by the
// control rather than attempted by the node.
ParameterUi CountUi (const char* section, int order, const char* help, int minimum, int maximum);

ParameterUi BooleanUi (const char* section, int order, const char* help);

} // namespace evp::nodegraph

#endif
