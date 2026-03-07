/*
    srtla_rec - SRT transport proxy with link aggregation
    Stats HTTP server

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <cstdint>
#include <thread>
#include <atomic>

void stats_server_start(uint16_t port, const char *geoip_db_path = nullptr);
void stats_server_stop();
