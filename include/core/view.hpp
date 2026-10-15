#pragma once

#include <vector>
#include <string>
#include "core/entry.hpp"

namespace core
{
    class View
    {
        public:
        virtual void RenderView(const std::vector<Entry>& entries) = 0;
        virtual void RenderTags(std::vector<std::string> tags) = 0;
    };
}