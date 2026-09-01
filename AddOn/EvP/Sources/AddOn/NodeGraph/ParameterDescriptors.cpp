#include "NodeGraph/ParameterDescriptors.hpp"

namespace evp::nodegraph {

ParameterUi NumberUi (const char* section, int order, const char* help, int decimals, const char* decimalsFrom)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Number;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.decimals = decimals;
    // A sibling that governs the precision, when one does. The slider's range
    // fields follow the SAME decimals setting the value does: a slider showing
    // two decimals whose minimum box shows three is telling the user its range
    // is finer than its value can express, and the step it nudges by has to
    // agree with both or the arrows land on numbers the field then rounds away.
    if (decimalsFrom != nullptr)
        ui.decimalsParameter = decimalsFrom;
    return ui;
}

// A length in metres, typed rather than dragged: a wall is 0.3 and a site is
// 200, so there is no range a slider could span without being useless at one end.
ParameterUi LengthUi (const char* section, int order, const char* help)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Number;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.unit = "m";
    ui.decimals = 3;
    // Zero and below are refused by the node body, so the control should not
    // offer them either - but the BODY is what enforces it. See ClampAndRound's
    // note: a range a client honours is not a range.
    ui.minimum = 0.0;
    ui.step = 0.1;
    return ui;
}

// ⚠️ ANGLES ARE AUTHORED IN DEGREES AND COMPUTED IN RADIANS, AND THE CONVERSION
// HAPPENS IN THE NODE. Nobody types 1.5707963 for a right angle. The unit says
// which one the field is, and every angle-taking node converts on the way in -
// so a value that reads 90 anywhere it is shown really is a quarter turn.
ParameterUi AngleUi (const char* section, int order, const char* help)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Number;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.unit = "deg";
    ui.decimals = 2;
    ui.step = 15.0;
    return ui;
}

// A whole number with a floor and a ceiling. The ceiling is the point: every one
// of these drives an allocation, and a typed extra zero should be refused by the
// control rather than attempted by the node.
ParameterUi CountUi (const char* section, int order, const char* help, int minimum, int maximum)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Number;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.minimum = static_cast<double> (minimum);
    ui.maximum = static_cast<double> (maximum);
    ui.step = 1.0;
    ui.decimals = 0;
    return ui;
}

ParameterUi BooleanUi (const char* section, int order, const char* help)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Boolean;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    return ui;
}

// A direction, drawn as one row of three fields. Distinct from PointUi only in
// the widget, which is what tells a client it may offer a pick-in-model
// affordance for one and not for the other.
ParameterUi VectorUi (const char* section, int order, const char* help)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Vector;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.decimals = 3;
    ui.components = { "X", "Y", "Z" };
    return ui;
}

ParameterUi PointUi (const char* section, int order, const char* help)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Point;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.unit = "m";
    ui.decimals = 3;
    ui.components = { "X", "Y", "Z" };
    return ui;
}

} // namespace evp::nodegraph
