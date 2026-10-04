#ifndef EVP_PALETTE_PARAMCALENDARVALUE_HPP
#define EVP_PALETTE_PARAMCALENDARVALUE_HPP

#include <chrono>
#include <string>

namespace evp {
inline bool ParseCalendarValue (const std::string& text, int& year, int& month, int& day)
{
    if (text.size () != 10 || text[4] != '-' || text[7] != '-')
        return false;
    for (size_t i = 0; i < text.size (); ++i)
        if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9'))
            return false;
    year = std::stoi (text.substr (0, 4));
    month = std::stoi (text.substr (5, 2));
    day = std::stoi (text.substr (8, 2));
    return year >= 1902 && year <= 2037 &&
           std::chrono::year_month_day (std::chrono::year (year), std::chrono::month (unsigned (month)),
                                        std::chrono::day (unsigned (day)))
               .ok ();
}
} // namespace evp
#endif
