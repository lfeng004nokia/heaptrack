/*
 * Copyright 2015 Milian Wolff <mail@milianw.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "parser.h"

#include <ThreadWeaver/ThreadWeaver>
#include <KLocalizedString>

#include <QDebug>

#include "../accumulatedtracedata.h"

#include <vector>
#include <tuple>

using namespace std;

namespace {

// TODO: use QString directly
struct StringCache
{
    StringCache()
    {
        m_ipAddresses.reserve(16384);
    }

    QString func(const InstructionPointer& ip) const
    {
        if (ip.functionIndex) {
            // TODO: support removal of template arguments
            return stringify(ip.functionIndex);
        } else if (diffMode) {
            return i18n("<unresolved function>");
        } else {
            auto& ipAddr = m_ipAddresses[ip.instructionPointer];
            if (ipAddr.isEmpty()) {
                ipAddr = QLatin1String("0x") + QString::number(ip.instructionPointer, 16);
            }
            return ipAddr;
        }
    }

    QString file(const InstructionPointer& ip) const
    {
        if (ip.fileIndex) {
            return stringify(ip.fileIndex);
        } else {
            return {};
        }
    }

    QString module(const InstructionPointer& ip) const
    {
        return stringify(ip.moduleIndex);
    }

    QString stringify(const StringIndex index) const
    {
        if (!index || index.index > m_strings.size()) {
            return {};
        } else {
            return m_strings.at(index.index - 1);
        }
    }

    shared_ptr<LocationData> location(const InstructionPointer& ip) const
    {
        LocationData data = {func(ip), file(ip), module(ip), ip.line};
        auto it = lower_bound(m_locations.begin(), m_locations.end(), data);
        if (it != m_locations.end() && **it == data) {
            return *it;
        } else {
            auto interned = make_shared<LocationData>(data);
            m_locations.insert(it, interned);
            return interned;
        }
    }

    void update(const vector<string>& strings)
    {
        transform(strings.begin() + m_strings.size(), strings.end(),
                  back_inserter(m_strings), [] (const string& str) { return QString::fromStdString(str); });
    }

    vector<QString> m_strings;
    mutable QHash<uint64_t, QString> m_ipAddresses;
    mutable vector<shared_ptr<LocationData>> m_locations;

    bool diffMode = false;
};

struct ChartMergeData
{
    IpIndex ip;
    qint64 consumed;
    qint64 allocations;
    qint64 allocated;
    qint64 temporary;
    bool operator<(const IpIndex rhs) const
    {
        return ip < rhs;
    }
};

const uint64_t MAX_CHART_DATAPOINTS = 500; // TODO: make this configurable via the GUI

struct ParserData final : public AccumulatedTraceData
{
    ParserData()
    {
    }

    void updateStringCache()
    {
        stringCache.update(strings);
    }

    void prepareBuildCharts()
    {
        consumedChartData.rows.reserve(MAX_CHART_DATAPOINTS);
        allocatedChartData.rows.reserve(MAX_CHART_DATAPOINTS);
        allocationsChartData.rows.reserve(MAX_CHART_DATAPOINTS);
        temporaryChartData.rows.reserve(MAX_CHART_DATAPOINTS);
        // start off with null data at the origin
        consumedChartData.rows.push_back({});
        allocatedChartData.rows.push_back({});
        allocationsChartData.rows.push_back({});
        temporaryChartData.rows.push_back({});
        // index 0 indicates the total row
        consumedChartData.labels[0] = i18n("total");
        allocatedChartData.labels[0] = i18n("total");
        allocationsChartData.labels[0] = i18n("total");
        temporaryChartData.labels[0] = i18n("total");

        buildCharts = true;
        maxConsumedSinceLastTimeStamp = 0;
        vector<ChartMergeData> merged;
        merged.reserve(instructionPointers.size());
        // merge the allocation cost by instruction pointer
        // TODO: aggregate by function instead?
        // TODO: traverse the merged call stack up until the first fork
        for (const auto& alloc : allocations) {
            const auto ip = findTrace(alloc.traceIndex).ipIndex;
            auto it = lower_bound(merged.begin(), merged.end(), ip);
            if (it == merged.end() || it->ip != ip) {
                it = merged.insert(it, {ip, 0, 0, 0, 0});
            }
            it->consumed += alloc.peak; // we want to track the top peaks in the chart
            it->allocated += alloc.allocated;
            it->allocations += alloc.allocations;
            it->temporary += alloc.temporary;
        }
        // find the top hot spots for the individual data members and remember their IP and store the label
        auto findTopChartEntries = [&] (qint64 ChartMergeData::* member, int LabelIds::* label, ChartData* data) {
            sort(merged.begin(), merged.end(), [=] (const ChartMergeData& left, const ChartMergeData& right) {
                return left.*member > right.*member;
            });
            for (size_t i = 0; i < min(size_t(ChartRows::MAX_NUM_COST - 1), merged.size()); ++i) {
                const auto& alloc = merged[i];
                if (!(alloc.*member)) {
                    break;
                }
                const auto ip = alloc.ip;
                (labelIds[ip].*label) = i + 1;
                const auto function = stringCache.func(findIp(ip));
                data->labels[i + 1] = function;
            }
        };
        findTopChartEntries(&ChartMergeData::consumed, &LabelIds::consumed, &consumedChartData);
        findTopChartEntries(&ChartMergeData::allocated, &LabelIds::allocated, &allocatedChartData);
        findTopChartEntries(&ChartMergeData::allocations, &LabelIds::allocations, &allocationsChartData);
        findTopChartEntries(&ChartMergeData::temporary, &LabelIds::temporary, &temporaryChartData);
    }

    void handleTimeStamp(int64_t /*oldStamp*/, int64_t newStamp)
    {
        if (!buildCharts) {
            return;
        }
        maxConsumedSinceLastTimeStamp = max(maxConsumedSinceLastTimeStamp, totalCost.leaked);
        const int64_t diffBetweenTimeStamps = totalTime / MAX_CHART_DATAPOINTS;
        if (newStamp != totalTime && newStamp - lastTimeStamp < diffBetweenTimeStamps) {
            return;
        }
        const auto nowConsumed = maxConsumedSinceLastTimeStamp;
        maxConsumedSinceLastTimeStamp = 0;
        lastTimeStamp = newStamp;

        // create the rows
        auto createRow = [] (int64_t timeStamp, int64_t totalCost) {
            ChartRows row;
            row.timeStamp = timeStamp;
            row.cost[0] = totalCost;
            return row;
        };
        auto consumed = createRow(newStamp, nowConsumed);
        auto allocated = createRow(newStamp, totalCost.allocated);
        auto allocs = createRow(newStamp, totalCost.allocations);
        auto temporary = createRow(newStamp, totalCost.temporary);

        // if the cost is non-zero and the ip corresponds to a hotspot function selected in the labels,
        // we add the cost to the rows column
        auto addDataToRow = [] (int64_t cost, int labelId, ChartRows* rows) {
            if (!cost || labelId == -1) {
                return;
            }
            rows->cost[labelId] += cost;
        };
        for (const auto& alloc : allocations) {
            const auto ip = findTrace(alloc.traceIndex).ipIndex;
            auto it = labelIds.constFind(ip);
            if (it == labelIds.constEnd()) {
                continue;
            }
            const auto& labelIds = *it;
            addDataToRow(alloc.leaked, labelIds.consumed, &consumed);
            addDataToRow(alloc.allocated, labelIds.allocated, &allocated);
            addDataToRow(alloc.allocations, labelIds.allocations, &allocs);
            addDataToRow(alloc.temporary, labelIds.temporary, &temporary);
        }
        // add the rows for this time stamp
        consumedChartData.rows << consumed;
        allocatedChartData.rows << allocated;
        allocationsChartData.rows << allocs;
        temporaryChartData.rows << temporary;
    }

    void handleAllocation(const AllocationInfo& info, const AllocationIndex index)
    {
        maxConsumedSinceLastTimeStamp = max(maxConsumedSinceLastTimeStamp, totalCost.leaked);

        if (index.index == allocationInfoCounter.size()) {
            allocationInfoCounter.push_back({info, 1});
        } else {
            ++allocationInfoCounter[index.index].allocations;
        }
    }

    void handleDebuggee(const char* command)
    {
        debuggee = command;
    }

    string debuggee;

    struct CountedAllocationInfo
    {
        AllocationInfo info;
        int64_t allocations;
        bool operator<(const CountedAllocationInfo& rhs) const
        {
            return tie(info.size, allocations)
                 < tie(rhs.info.size, rhs.allocations);
        }
    };
    vector<CountedAllocationInfo> allocationInfoCounter;

    ChartData consumedChartData;
    ChartData allocationsChartData;
    ChartData allocatedChartData;
    ChartData temporaryChartData;
    // here we store the indices into ChartRows::cost for those IpIndices that
    // are within the top hotspots. This way, we can do one hash lookup in the
    // handleTimeStamp function instead of three when we'd store this data
    // in a per-ChartData hash.
    struct LabelIds
    {
        int consumed = -1;
        int allocations = -1;
        int allocated = -1;
        int temporary = -1;
    };
    QHash<IpIndex, LabelIds> labelIds;
    int64_t maxConsumedSinceLastTimeStamp = 0;
    int64_t lastTimeStamp = 0;

    StringCache stringCache;

    bool buildCharts = false;
};

void setParents(QVector<RowData>& children, const RowData* parent)
{
    for (auto& row: children) {
        row.parent = parent;
        setParents(row.children, &row);
    }
}

QPair<TreeData, CallerCalleeRows> mergeAllocations(const ParserData& data)
{
    TreeData topRows;
    CallerCalleeRows callerCalleeRows;
    callerCalleeRows.reserve(data.instructionPointers.size());
    // merge allocations, leave parent pointers invalid (their location may change)
    for (const auto& allocation : data.allocations) {
        auto traceIndex = allocation.traceIndex;
        auto rows = &topRows;
        while (traceIndex) {
            const auto& trace = data.findTrace(traceIndex);
            const auto& ip = data.findIp(trace.ipIndex);
            auto location = data.stringCache.location(ip);

            { // aggregate caller-callee data
                auto it = lower_bound(callerCalleeRows.begin(), callerCalleeRows.end(), location,
                                      [] (const CallerCalleeData& lhs, const std::shared_ptr<LocationData>& rhs) {
                    return lhs.location < rhs;
                });
                if (it == callerCalleeRows.end() || it->location != location) {
                    it = callerCalleeRows.insert(it, {{}, {}, location});
                }
                it->inclusiveCost += allocation;
                if (traceIndex == allocation.traceIndex) {
                    it->selfCost += allocation;
                }
            }

            auto it = lower_bound(rows->begin(), rows->end(), location);
            if (it != rows->end() && it->location == location) {
                it->cost += allocation;
            } else {
                it = rows->insert(it, {allocation, location, nullptr, {}});
            }
            if (data.isStopIndex(ip.functionIndex)) {
                break;
            }
            traceIndex = trace.parentIndex;
            rows = &it->children;
        }
    }
    // now set the parents, the data is constant from here on
    setParents(topRows, nullptr);
    return qMakePair(topRows, callerCalleeRows);
}

RowData* findByLocation(const RowData& row, QVector<RowData>* data)
{
    for (int i = 0; i < data->size(); ++i) {
        if (data->at(i).location == row.location) {
            return data->data() + i;
        }
    }
    return nullptr;
}

void buildTopDown(const TreeData& bottomUpData, TreeData* topDownData)
{
    foreach (const auto& row, bottomUpData) {
        if (row.children.isEmpty()) {
            // leaf node found, bubble up the parent chain to build a top-down tree
            auto node = &row;
            auto stack = topDownData;
            while (node) {
                auto data = findByLocation(*node, stack);
                if (!data) {
                    // create an empty top-down item for this bottom-up node
                    *stack << RowData{{}, node->location, nullptr, {}};
                    data = &stack->back();
                }
                // always use the leaf node's cost and propagate that one up the chain
                // otherwise we'd count the cost of some nodes multiple times
                data->cost += row.cost;
                stack = &data->children;
                node = node->parent;
            }
        } else {
            // recurse to find a leaf
            buildTopDown(row.children, topDownData);
        }
    }
}

QVector<RowData> toTopDownData(const QVector<RowData>& bottomUpData)
{
    QVector<RowData> topRows;
    buildTopDown(bottomUpData, &topRows);
    // now set the parents, the data is constant from here on
    setParents(topRows, nullptr);
    return topRows;
}

struct MergedHistogramColumnData
{
    std::shared_ptr<LocationData> location;
    int64_t allocations;
    bool operator<(const std::shared_ptr<LocationData>& rhs) const
    {
        return location < rhs;
    }
};

HistogramData buildSizeHistogram(ParserData& data)
{
    HistogramData ret;
    if (data.allocationInfoCounter.empty()) {
        return ret;
    }
    sort(data.allocationInfoCounter.begin(), data.allocationInfoCounter.end());
    const auto totalLabel = i18n("total");
    HistogramRow row;
    const pair<uint64_t, QString> buckets[] = {
        {8, i18n("0B to 8B")},
        {16, i18n("9B to 16B")},
        {32, i18n("17B to 32B")},
        {64, i18n("33B to 64B")},
        {128, i18n("65B to 128B")},
        {256, i18n("129B to 256B")},
        {512, i18n("257B to 512B")},
        {1024, i18n("512B to 1KB")},
        {numeric_limits<uint64_t>::max(), i18n("more than 1KB")}
    };
    uint bucketIndex = 0;
    row.size = buckets[bucketIndex].first;
    row.sizeLabel = buckets[bucketIndex].second;
    vector<MergedHistogramColumnData> columnData;
    columnData.reserve(128);
    auto insertColumns = [&] () {
        sort(columnData.begin(), columnData.end(), [] (const MergedHistogramColumnData& lhs, const MergedHistogramColumnData& rhs) {
            return lhs.allocations > rhs.allocations;
        });
        // -1 to account for total row
        for (size_t i = 0; i < min(columnData.size(), size_t(HistogramRow::NUM_COLUMNS - 1)); ++i) {
            const auto& column = columnData[i];
            row.columns[i + 1] = {column.allocations, column.location};
        }
    };
    for (const auto& info : data.allocationInfoCounter) {
        if (info.info.size > row.size) {
            insertColumns();
            columnData.clear();
            ret << row;
            ++bucketIndex;
            row.size = buckets[bucketIndex].first;
            row.sizeLabel = buckets[bucketIndex].second;
            row.columns[0] = {info.allocations, {}};
        } else {
            row.columns[0].allocations += info.allocations;
        }
        const auto ipIndex = data.findTrace(info.info.traceIndex).ipIndex;
        const auto ip = data.findIp(ipIndex);
        const auto location = data.stringCache.location(ip);
        auto it = lower_bound(columnData.begin(), columnData.end(), location);
        if (it == columnData.end() || it->location != location) {
            columnData.insert(it, {location, info.allocations});
        } else {
            it->allocations += info.allocations;
        }
    }
    insertColumns();
    ret << row;
    return ret;
}

}

Parser::Parser(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<SummaryData>();
}

Parser::~Parser() = default;

void Parser::parse(const QString& path, const QString& diffBase)
{
    using namespace ThreadWeaver;
    stream() << make_job([this, path, diffBase]() {
        const auto stdPath = path.toStdString();
        auto data = make_shared<ParserData>();
        emit progressMessageAvailable(i18n("parsing data..."));
        if (!data->read(stdPath)) {
            emit failedToOpen(path);
            return;
        }

        if (!diffBase.isEmpty()) {
            ParserData diffData;
            if (!diffData.read(diffBase.toStdString())) {
                emit failedToOpen(diffBase);
                return;
            }
            data->diff(diffData);
            data->stringCache.diffMode = true;
        }

        data->updateStringCache();

        emit summaryAvailable({
            QString::fromStdString(data->debuggee),
            data->totalCost,
            data->totalTime,
            data->peakTime,
            data->peakRSS * data->systemInfo.pageSize,
            data->systemInfo.pages * data->systemInfo.pageSize
        });

        emit progressMessageAvailable(i18n("merging allocations..."));
        // merge allocations before modifying the data again
        const auto mergedAllocations = mergeAllocations(*data);
        emit bottomUpDataAvailable(mergedAllocations.first);
        emit callerCalleeDataAvailable(mergedAllocations.second);

        // also calculate the size histogram
        emit progressMessageAvailable(i18n("building size histogram..."));
        const auto sizeHistogram = buildSizeHistogram(*data);
        emit sizeHistogramDataAvailable(sizeHistogram);
        // now data can be modified again for the chart data evaluation

        emit progressMessageAvailable(i18n("building charts..."));
        auto parallel = new Collection;
        *parallel << make_job([this, mergedAllocations, sizeHistogram]() {
            const auto topDownData = toTopDownData(mergedAllocations.first);
            emit topDownDataAvailable(topDownData);
        }) << make_job([this, data, stdPath]() {
            // this mutates data, and thus anything running in parallel must
            // not access data
            data->prepareBuildCharts();
            data->read(stdPath);
            emit consumedChartDataAvailable(data->consumedChartData);
            emit allocationsChartDataAvailable(data->allocationsChartData);
            emit allocatedChartDataAvailable(data->allocatedChartData);
            emit temporaryChartDataAvailable(data->temporaryChartData);
        });

        auto sequential = new Sequence;
        *sequential << parallel << make_job([this]() {
            emit finished();
        });

        stream() << sequential;
    });
}
