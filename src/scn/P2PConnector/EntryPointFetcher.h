/*
 * This file is part of SwabianCoin.
 *
 * SwabianCoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * SwabianCoin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SwabianCoin.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef FULL_NODE_ENTRYPOINTFETCHER_H
#define FULL_NODE_ENTRYPOINTFETCHER_H

#include "IEntryPointFetcher.h"

namespace scn {

    class EntryPointFetcher : public IEntryPointFetcher {
    public:
        EntryPointFetcher() = default;
        virtual ~EntryPointFetcher() = default;

        const std::list<std::pair<std::string, std::uint16_t>> fetch() override;

    protected:
        static const std::string entry_points_host_;
        static const std::string entry_points_path_;
        static const std::string ca_cert_path_;
        static const std::list<std::pair<std::string, std::uint16_t>> entry_points_fallback_;
    };

}

#endif //FULL_NODE_ENTRYPOINTFETCHER_H
