/******************************************************************************
 * Copyright (c) 2018, Connor Manning
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following
 * conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of the Martin Isenburg or Iowa Department
 *       of Natural Resources nor the names of its contributors may be
 *       used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 ****************************************************************************/

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include <pdal/pdal_export.hpp>
#include <pdal/pdal_types.hpp>
#include <pdal/PointLayout.hpp>
#include <pdal/PointTable.hpp>
#include <pdal/Stage.hpp>
#include <pdal/util/Bounds.hpp>
#include <pdal/util/Utils.hpp>

#include <arbiter/arbiter.hpp>

namespace pdal
{

class ept_error : public pdal_error
{
public:
    ept_error(std::string msg) : pdal_error(msg) { }
};

inline Dimension::Type getType(const Json::Value& dim)
{
    if (dim.isMember("scale"))
        return Dimension::Type::Double;

    const std::string type(dim["type"].asString());
    const uint64_t size(dim["size"].asUInt64());

    if (type == "signed")
    {
        switch (size)
        {
            case 1: return Dimension::Type::Signed8;
            case 2: return Dimension::Type::Signed16;
            case 4: return Dimension::Type::Signed32;
            case 8: return Dimension::Type::Signed64;
        }
    }
    else if (type == "unsigned")
    {
        switch (size)
        {
            case 1: return Dimension::Type::Unsigned8;
            case 2: return Dimension::Type::Unsigned16;
            case 4: return Dimension::Type::Unsigned32;
            case 8: return Dimension::Type::Unsigned64;
        }
    }
    else if (type == "float")
    {
        switch (size)
        {
            case 4: return Dimension::Type::Float;
            case 8: return Dimension::Type::Double;
        }
    }

    return Dimension::Type::None;
}

inline std::string stringify(const Json::Value& json)
{
    Json::StreamWriterBuilder builder;
    builder.settings_["indentation"] = "";
    return Json::writeString(builder, json);
}

inline Json::Value parse(const std::string& data)
{
    Json::Value json;
    Json::Reader reader;

    if (!reader.parse(data, json, false))
    {
        const std::string jsonError(reader.getFormattedErrorMessages());
        if (!jsonError.empty())
        {
            throw ept_error("Error during parsing: " + jsonError);
        }
    }

    return json;
}

inline BOX3D toBox3d(const Json::Value& b)
{
    if (!b.isArray() || b.size() != 6)
    {
        throw ept_error("Invalid bounds specification: " + b.toStyledString());
    }

    return BOX3D(b[0].asDouble(), b[1].asDouble(), b[2].asDouble(),
            b[3].asDouble(), b[4].asDouble(), b[5].asDouble());
}

class PDAL_DLL Key
{
    // An EPT key representation (see https://git.io/fAiBh).  A depth/X/Y/Z key
    // representing a data node, as well as the bounds of the contained data.
public:
    Key() { }
    Key(std::string s)
    {
        const std::vector<std::string> tokens(Utils::split(s, '-'));
        if (tokens.size() != 4)
            throw ept_error("Invalid EPT KEY: " + s);
        d = std::stoull(tokens[0]);
        x = std::stoull(tokens[1]);
        y = std::stoull(tokens[2]);
        z = std::stoull(tokens[3]);
    }

    BOX3D b;
    uint64_t d = 0;
    uint64_t x = 0;
    uint64_t y = 0;
    uint64_t z = 0;

    std::string toString() const
    {
        return std::to_string(d) + '-' + std::to_string(x) + '-' +
            std::to_string(y) + '-' + std::to_string(z);
    }

    double& operator[](uint64_t i)
    {
        switch (i)
        {
            case 0: return b.minx;
            case 1: return b.miny;
            case 2: return b.minz;
            case 3: return b.maxx;
            case 4: return b.maxy;
            case 5: return b.maxz;
            default: throw ept_error("Invalid Key[] index");
        }
    }

    uint64_t& idAt(uint64_t i)
    {
        switch (i)
        {
            case 0: return x;
            case 1: return y;
            case 2: return z;
            default: throw ept_error("Invalid Key::idAt index");
        }
    }

    Key bisect(uint64_t direction) const
    {
        Key key(*this);
        ++key.d;

        auto step([&key, direction](uint8_t i)
        {
            key.idAt(i) *= 2;

            const double mid(key[i] + (key[i + 3] - key[i]) / 2.0);
            const bool positive(direction & (((uint64_t)1) << i));
            if (positive)
            {
                key[i] = mid;
                ++key.idAt(i);
            }
            else
            {
                key[i + 3] = mid;
            }
        });

        for (uint8_t i(0); i < 3; ++i)
            step(i);

        return key;
    }
};

using EptHierarchy = std::map<Key, uint64_t>;

class PDAL_DLL Addon
{
public:
    Addon(const PointLayout& layout, const arbiter::Endpoint& ep,
            Dimension::Id id)
        : m_ep(ep)
        , m_id(id)
        , m_type(layout.dimType(m_id))
        , m_size(layout.dimSize(m_id))
        , m_name(layout.dimName(m_id))
    { }

    const arbiter::Endpoint& ep() const { return m_ep; }
    Dimension::Id id() const { return m_id; }
    Dimension::Type type() const { return m_type; }
    uint64_t size() const { return m_size; }
    const std::string& name() const { return m_name; }

    EptHierarchy& hierarchy() { return m_hierarchy; }
    uint64_t points(const Key& key) const
    {
        return m_hierarchy.count(key) ? m_hierarchy.at(key) : 0;
    }

private:
    const arbiter::Endpoint m_ep;

    const Dimension::Id m_id = Dimension::Id::Unknown;
    const Dimension::Type m_type = Dimension::Type::None;
    const uint64_t m_size = 0;
    const std::string m_name;

    EptHierarchy m_hierarchy;
};

class PDAL_DLL EptInfo
{
public:
    enum class DataType
    {
        Laszip,
        Binary
    };

    EptInfo(Json::Value info) : m_info(info)
    {
        m_bounds = toBox3d(m_info["bounds"]);
        m_points = m_info["points"].asUInt64();
        m_span = m_info["span"].asUInt64();
        m_srs = m_info["srs"]["wkt"].asString();

        if (m_srs.empty())
        {
            if (m_info["srs"].isMember("authority") &&
                    m_info["srs"].isMember("horizontal"))
            {
                m_srs = m_info["srs"]["authority"].asString() + ":" +
                    m_info["srs"]["horizontal"].asString();
            }

            if (m_info["srs"].isMember("vertical"))
            {
                m_srs += "+" + m_info["srs"]["vertical"].asString();
            }
        }

        const std::string dt(m_info["dataType"].asString());
        if (dt == "laszip")
            m_dataType = DataType::Laszip;
        else if (dt == "binary")
            m_dataType = DataType::Binary;
        else
            throw ept_error("Unrecognized EPT dataType: " + dt);
    }

    const BOX3D& bounds() const { return m_bounds; }
    uint64_t points() const { return m_points; }
    uint64_t span() const { return m_span; }
    DataType dataType() const { return m_dataType; }
    const std::string& srs() const { return m_srs; }
    const Json::Value& schema() const { return m_info["schema"]; }
    const Json::Value dim(std::string name) const
    {
        for (Json::ArrayIndex i(0); i < schema().size(); ++i)
        {
            if (schema()[i]["name"].asString() == name)
            {
                return schema()[i];
            }
        }
        return Json::nullValue;
    }

    uint64_t sources() const { return m_info["sources"].asUInt64(); }

    const Json::Value& json() { return m_info; }

private:
    // Info comes from the values here:
    // https://entwine.io/entwine-point-tile.html#ept-json
    const Json::Value m_info;
    BOX3D m_bounds;
    uint64_t m_points = 0;

    // The span is the length, width, and depth of the octree grid.  For
    // example, a dataset oriented as a 256*256*256 octree grid would have a
    // span of 256.
    //
    // See: https://entwine.io/entwine-point-tile.html#span
    uint64_t m_span = 0;

    DataType m_dataType;
    std::string m_srs;
};

inline bool operator<(const Key& a, const Key& b)
{
    if (a.d < b.d) return true;
    if (a.d > b.d) return false;

    if (a.x < b.x) return true;
    if (a.x > b.x) return false;

    if (a.y < b.y) return true;
    if (a.y > b.y) return false;

    if (a.z < b.z) return true;
    return false;
}

class FixedPointLayout : public PointLayout
{
    // The default PointLayout class may reorder dimension entries for packing
    // efficiency.  However if a PointLayout is intended to be mapped to data
    // coming from a remote source, then the dimensions must retain their order.
    // FixedPointLayout retains the order of dimensions as they are registered.
protected:
    PDAL_DLL virtual bool update(
            pdal::Dimension::Detail dimDetail,
            const std::string& name) override
    {
        if (!m_finalized)
        {
            if (!contains(m_used, dimDetail.id()))
            {
                dimDetail.setOffset(m_pointSize);

                m_pointSize += dimDetail.size();
                m_used.push_back(dimDetail.id());
                m_detail[Utils::toNative(dimDetail.id())] = dimDetail;

                return true;
            }
        }
        else return m_propIds.count(name);

        return false;
    }

    PDAL_DLL bool contains(
            const Dimension::IdList& idList,
            const Dimension::Id id) const
    {
        for (const auto current : idList)
        {
            if (current == id) return true;
        }

        return false;
    }
};

class PDAL_DLL ShallowPointTable : public BasePointTable
{
    // PointTable semantics around a raw buffer of data matching the specified
    // layout.  Intended for accessing data from a remote source.
public:
    ShallowPointTable(PointLayout& layout, char* data, std::size_t size)
        : BasePointTable(layout)
        , m_data(data)
        , m_size(size)
    {}

    std::size_t numPoints() const { return m_size / layout()->pointSize(); }

protected:
    virtual PointId addPoint() override
    {
        throw ept_error("Cannot add points to ShallowPointTable");
    }

    virtual char* getPoint(PointId i) override
    {
        return m_data + i * layout()->pointSize();
    }

    // Identical to SimplePointTable's implementation.
    void setFieldInternal(Dimension::Id id, PointId idx, const void* value)
        override
    {
        const Dimension::Detail* d = layout()->dimDetail(id);
        const char* src  = (const char*)value;
        char* dst = getDimension(d, idx);
        std::copy(src, src + d->size(), dst);
    }

    void getFieldInternal(Dimension::Id id, PointId idx, void* value) const
        override
    {
        const Dimension::Detail* d = layout()->dimDetail(id);
        const char* src = getDimension(d, idx);
        char* dst = (char*)value;
        std::copy(src, src + d->size(), dst);
    }

    char *getDimension(const Dimension::Detail* d, PointId idx)
    {
        return getPoint(idx) + d->offset();
    }

    const char *getDimension(const Dimension::Detail* d, PointId idx) const
    {
        ShallowPointTable* ncThis = const_cast<ShallowPointTable*>(this);
        return ncThis->getPoint(idx) + d->offset();
    }

    char* m_data;
    std::size_t m_size;
};

class PDAL_DLL Pool
{
public:
    // After numThreads tasks are actively running, and queueSize tasks have
    // been enqueued to wait for an available worker thread, subsequent calls
    // to Pool::add will block until an enqueued task has been popped from the
    // queue.
    Pool(
            std::size_t numThreads,
            std::size_t queueSize = 1,
            bool verbose = true)
        : m_verbose(verbose)
        , m_numThreads(std::max<std::size_t>(numThreads, 1))
        , m_queueSize(std::max<std::size_t>(queueSize, 1))
    {
        go();
    }

    ~Pool() { join(); }

    // Start worker threads.
    void go()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return;
        m_running = true;

        for (std::size_t i(0); i < m_numThreads; ++i)
        {
            m_threads.emplace_back([this]() { work(); });
        }
    }

    // Disallow the addition of new tasks and wait for all currently running
    // tasks to complete.
    void join()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;
        lock.unlock();

        m_consumeCv.notify_all();
        for (auto& t : m_threads) t.join();
        m_threads.clear();
    }

    // Wait for all current tasks to complete.  As opposed to join, tasks may
    // continue to be added while a thread is await()-ing the queue to empty.
    void await()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_produceCv.wait(lock, [this]()
        {
            return !m_outstanding && m_tasks.empty();
        });
    }

    // Join and restart.
    void cycle() { join(); go(); }

    // Change the number of threads.  Current threads will be joined.
    void resize(const std::size_t numThreads)
    {
        join();
        m_numThreads = numThreads;
        go();
    }

    // Not thread-safe, pool should be joined before calling.
    const std::vector<std::string>& errors() const { return m_errors; }

    // Add a threaded task, blocking until a thread is available.  If join() is
    // called, add() may not be called again until go() is called and completes.
    void add(std::function<void()> task)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_running)
        {
            throw ept_error("Attempted to add a task to a stopped Pool");
        }

        m_produceCv.wait(lock, [this]()
        {
            return m_tasks.size() < m_queueSize;
        });

        m_tasks.emplace(task);

        // Notify worker that a task is available.
        lock.unlock();
        m_consumeCv.notify_all();
    }


    std::size_t size() const { return m_numThreads; }
    std::size_t numThreads() const { return m_numThreads; }

private:
    // Worker thread function.  Wait for a task and run it - or if stop() is
    // called, complete any outstanding task and return.
    void work()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_consumeCv.wait(lock, [this]()
            {
                return m_tasks.size() || !m_running;
            });

            if (m_tasks.size())
            {
                ++m_outstanding;
                auto task(std::move(m_tasks.front()));
                m_tasks.pop();

                lock.unlock();

                // Notify add(), which may be waiting for a spot in the queue.
                m_produceCv.notify_all();

                std::string err;
                try { task(); }
                catch (std::exception& e) { err = e.what(); }
                catch (...) { err = "Unknown error"; }

                lock.lock();
                --m_outstanding;
                if (err.size())
                {
                    if (m_verbose)
                    {
                        std::cout << "Exception in pool task: " << err <<
                            std::endl;
                    }
                    m_errors.push_back(err);
                }
                lock.unlock();

                // Notify await(), which may be waiting for a running task.
                m_produceCv.notify_all();
            }
            else if (!m_running)
            {
                return;
            }
        }
    }

    bool m_verbose;
    std::size_t m_numThreads;
    std::size_t m_queueSize;
    std::vector<std::thread> m_threads;
    std::queue<std::function<void()>> m_tasks;

    std::vector<std::string> m_errors;
    std::mutex m_errorMutex;

    std::size_t m_outstanding = 0;
    bool m_running = false;

    mutable std::mutex m_mutex;
    std::condition_variable m_produceCv;
    std::condition_variable m_consumeCv;

    // Disable copy/assignment.
    Pool(const Pool& other);
    Pool& operator=(const Pool& other);
};

} // namespace pdal

