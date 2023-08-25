// Small utility for std::variant
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef OVERLOADED_H
#define OVERLOADED_H

namespace
{
    template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
}

#endif
