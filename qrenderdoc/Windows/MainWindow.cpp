/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "MainWindow.h"
#include <algorithm>
#include <atomic>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmapCache>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QShortcut>
#include <QTextStream>
#include <QToolButton>
#include <QToolTip>
#include "Code/QRDUtils.h"
#include "Code/Resources.h"
#include "Widgets/Extended/RDLabel.h"
#include "Widgets/Extended/RDMenu.h"
#include "Widgets/ReplayOptionsSelector.h"
#include "Windows/Dialogs/AboutDialog.h"
#include "Windows/Dialogs/CaptureDialog.h"
#include "Windows/Dialogs/CrashDialog.h"
#include "Windows/Dialogs/ExtensionManager.h"
#include "Windows/Dialogs/LiveCapture.h"
#include "Windows/Dialogs/RemoteManager.h"
#include "Windows/Dialogs/SettingsDialog.h"
#include "Windows/Dialogs/SuggestRemoteDialog.h"
#include "Windows/Dialogs/TipsDialog.h"
#include "Windows/Dialogs/UpdateDialog.h"
#include "ui_MainWindow.h"
#include "version.h"

#define JSON_ID "rdocLayoutData"
#define JSON_VER 1

#if defined(Q_OS_WIN32)
extern "C" void *__stdcall GetModuleHandleA(const char *);
#endif

namespace
{
static const int ExportPathComponentLength = 48;
static const int MaxHotspotCount = 50;

struct ExportActionInfo
{
  uint32_t actionId = 0;
  uint32_t eventId = 0;
  QString name;
  QString hierarchy;
  QString kind;
  QString flagsText;
  bool fakeMarker = false;
  ActionFlags flags = ActionFlags::NoFlags;
  uint32_t numIndices = 0;
  uint32_t numInstances = 0;
  int32_t baseVertex = 0;
  uint32_t indexOffset = 0;
  uint32_t vertexOffset = 0;
  uint32_t instanceOffset = 0;
  uint32_t drawIndex = 0;
  rdcfixedarray<uint32_t, 3> dispatchDimension = {0, 0, 0};
  rdcfixedarray<uint32_t, 3> dispatchThreadsDimension = {0, 0, 0};
  rdcfixedarray<uint32_t, 3> dispatchBase = {0, 0, 0};
  ResourceId copySource;
  ResourceId copyDestination;
  ResourceId depthOut;
  rdcfixedarray<ResourceId, 8> outputs;
  QVector<uint32_t> eventIds;
};

struct ExportActionSummary
{
  ExportActionInfo action;
  double gpuDurationMs = -1.0;
  int counterValueCount = 0;
  int performanceMessageCount = 0;
};

QString SanitiseFileComponent(QString value, int maxLen = ExportPathComponentLength)
{
  value = value.simplified();
  value.replace(QRegularExpression(lit("[\\\\/:*?\"<>|]+")), lit("_"));
  value.replace(QRegularExpression(lit("\\s+")), lit("_"));
  value.replace(QRegularExpression(lit("[^A-Za-z0-9._@+-]+")), lit("_"));
  value.remove(QRegularExpression(lit("^_+")));
  value.remove(QRegularExpression(lit("_+$")));

  if(value.isEmpty())
    value = lit("unnamed");

  if(value.size() > maxLen)
    value = value.left(maxLen);

  return value;
}

QString ResourceIdText(ResourceId id)
{
  return ToQStr(id);
}

QString FromRDCStr(const rdcstr &value)
{
  return QString(value);
}

QString ActionKind(ActionFlags flags)
{
  if(flags & ActionFlags::Drawcall)
    return lit("Drawcall");
  if(flags & ActionFlags::MeshDispatch)
    return lit("MeshDispatch");
  if(flags & ActionFlags::Dispatch)
    return lit("Dispatch");
  if(flags & ActionFlags::DispatchRay)
    return lit("DispatchRay");
  if(flags & ActionFlags::Copy)
    return lit("Copy");
  if(flags & ActionFlags::Resolve)
    return lit("Resolve");
  if(flags & ActionFlags::Clear)
    return lit("Clear");
  if(flags & ActionFlags::Present)
    return lit("Present");
  if(flags & ActionFlags::PushMarker)
    return lit("PushMarker");
  if(flags & ActionFlags::SetMarker)
    return lit("SetMarker");
  if(flags & ActionFlags::PopMarker)
    return lit("PopMarker");
  if(flags & ActionFlags::CmdList)
    return lit("CommandBuffer");
  return lit("Action");
}

QString DisplayActionName(const ActionDescription &action, const SDFile &structuredFile)
{
  QString name = FromRDCStr(action.GetName(structuredFile)).trimmed();
  if(name.isEmpty())
    name = QFormatStr("%1_E%2").arg(ActionKind(action.flags)).arg(action.eventId);
  return name;
}

QJsonValue ToJsonU64(uint64_t value)
{
  return QString::number(value);
}

QJsonArray ToJsonArray(const QVector<uint32_t> &values)
{
  QJsonArray arr;
  for(uint32_t value : values)
    arr.append(int(value));
  return arr;
}

QJsonArray ToJsonArray(const rdcfixedarray<uint32_t, 3> &values)
{
  QJsonArray arr;
  arr.append(int(values[0]));
  arr.append(int(values[1]));
  arr.append(int(values[2]));
  return arr;
}

bool WriteTextFile(const QString &path, const QString &contents, QString &error)
{
  QFile file(path);
  if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    error = QFormatStr("Couldn't open '%1' for writing: %2").arg(path).arg(file.errorString());
    return false;
  }

  QByteArray data = contents.toUtf8();
  if(file.write(data) != data.size())
  {
    error = QFormatStr("Couldn't write '%1': %2").arg(path).arg(file.errorString());
    return false;
  }

  return true;
}

bool WriteJsonFile(const QString &path, const QJsonObject &json, QString &error)
{
  QFile file(path);
  if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    error = QFormatStr("Couldn't open '%1' for writing: %2").arg(path).arg(file.errorString());
    return false;
  }

  QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Indented);
  if(file.write(data) != data.size())
  {
    error = QFormatStr("Couldn't write '%1': %2").arg(path).arg(file.errorString());
    return false;
  }

  return true;
}

QJsonObject MakeDebugMessageJson(const DebugMessage &message)
{
  QJsonObject obj;
  obj[lit("event_id")] = int(message.eventId);
  obj[lit("category")] = ToQStr(message.category);
  obj[lit("severity")] = ToQStr(message.severity);
  obj[lit("source")] = ToQStr(message.source);
  obj[lit("message_id")] = int(message.messageID);
  obj[lit("description")] = FromRDCStr(message.description);
  return obj;
}

QJsonObject MakeResourceSummaryJson(const QString &label, ResourceId id,
                                    const QMap<ResourceId, QString> &resourceNames,
                                    const QMap<ResourceId, ResourceDescription> &resourceDescs,
                                    const QMap<ResourceId, TextureDescription> &textureDescs,
                                    const QMap<ResourceId, BufferDescription> &bufferDescs)
{
  QJsonObject obj;

  if(id == ResourceId())
    return obj;

  obj[lit("label")] = label;
  obj[lit("resource_id")] = ResourceIdText(id);

  QString resourceName = resourceNames.value(id).trimmed();
  obj[lit("name")] = resourceName.isEmpty() ? ResourceIdText(id) : resourceName;

  auto resIt = resourceDescs.find(id);
  if(resIt != resourceDescs.end())
    obj[lit("resource_type")] = ToQStr(resIt.value().type);

  auto texIt = textureDescs.find(id);
  if(texIt != textureDescs.end())
  {
    const TextureDescription &tex = texIt.value();
    obj[lit("kind")] = lit("texture");
    obj[lit("texture_type")] = ToQStr(tex.type);
    obj[lit("dimension")] = int(tex.dimension);
    obj[lit("width")] = int(tex.width);
    obj[lit("height")] = int(tex.height);
    obj[lit("depth")] = int(tex.depth);
    obj[lit("array_size")] = int(tex.arraysize);
    obj[lit("mips")] = int(tex.mips);
    obj[lit("msaa_samples")] = int(tex.msSamp);
    obj[lit("format")] = FromRDCStr(tex.format.Name());
    obj[lit("byte_size")] = ToJsonU64(tex.byteSize);
    obj[lit("creation_flags")] = ToQStr(tex.creationFlags);
    return obj;
  }

  auto bufIt = bufferDescs.find(id);
  if(bufIt != bufferDescs.end())
  {
    const BufferDescription &buf = bufIt.value();
    obj[lit("kind")] = lit("buffer");
    obj[lit("length")] = ToJsonU64(buf.length);
    obj[lit("gpu_address")] = ToJsonU64(buf.gpuAddress);
    obj[lit("creation_flags")] = ToQStr(buf.creationFlags);
    return obj;
  }

  obj[lit("kind")] = lit("resource");
  return obj;
}

QJsonObject MakeBasicBindStatsJson(uint32_t calls, uint32_t sets, uint32_t nulls)
{
  QJsonObject obj;
  obj[lit("calls")] = int(calls);
  obj[lit("sets")] = int(sets);
  obj[lit("nulls")] = int(nulls);
  return obj;
}

QJsonObject MakeStateBindStatsJson(uint32_t calls, uint32_t sets, uint32_t nulls,
                                   uint32_t redundants)
{
  QJsonObject obj = MakeBasicBindStatsJson(calls, sets, nulls);
  obj[lit("redundants")] = int(redundants);
  return obj;
}

QJsonObject MakeFrameStatsJson(const FrameStatistics &stats)
{
  QJsonObject obj;
  obj[lit("recorded")] = stats.recorded;

  if(!stats.recorded)
    return obj;

  QJsonObject draws;
  draws[lit("calls")] = int(stats.draws.calls);
  draws[lit("instanced")] = int(stats.draws.instanced);
  draws[lit("indirect")] = int(stats.draws.indirect);
  obj[lit("draws")] = draws;

  QJsonObject dispatches;
  dispatches[lit("calls")] = int(stats.dispatches.calls);
  dispatches[lit("indirect")] = int(stats.dispatches.indirect);
  obj[lit("dispatches")] = dispatches;

  QJsonObject updates;
  updates[lit("calls")] = int(stats.updates.calls);
  updates[lit("client_writes")] = int(stats.updates.clients);
  updates[lit("server_writes")] = int(stats.updates.servers);
  obj[lit("resource_updates")] = updates;

  obj[lit("index_binds")] =
      MakeBasicBindStatsJson(stats.indices.calls, stats.indices.sets, stats.indices.nulls);
  obj[lit("vertex_binds")] =
      MakeBasicBindStatsJson(stats.vertices.calls, stats.vertices.sets, stats.vertices.nulls);
  obj[lit("layout_binds")] =
      MakeBasicBindStatsJson(stats.layouts.calls, stats.layouts.sets, stats.layouts.nulls);
  obj[lit("output_binds")] =
      MakeBasicBindStatsJson(stats.outputs.calls, stats.outputs.sets, stats.outputs.nulls);
  obj[lit("blend_state_binds")] = MakeStateBindStatsJson(stats.blends.calls, stats.blends.sets,
                                                         stats.blends.nulls, stats.blends.redundants);
  obj[lit("depth_stencil_binds")] = MakeStateBindStatsJson(
      stats.depths.calls, stats.depths.sets, stats.depths.nulls, stats.depths.redundants);
  obj[lit("rasterizer_binds")] = MakeStateBindStatsJson(
      stats.rasters.calls, stats.rasters.sets, stats.rasters.nulls, stats.rasters.redundants);

  return obj;
}

QJsonObject MakeCounterDescriptionJson(const CounterDescription &desc)
{
  QJsonObject obj;
  obj[lit("counter")] = ToQStr(desc.counter);
  obj[lit("name")] = FromRDCStr(desc.name);
  obj[lit("category")] = FromRDCStr(desc.category);
  obj[lit("description")] = FromRDCStr(desc.description);
  obj[lit("result_type")] = ToQStr(desc.resultType);
  obj[lit("result_byte_width")] = int(desc.resultByteWidth);
  obj[lit("unit")] = ToQStr(desc.unit);
  return obj;
}

QJsonValue MakeCounterValueJson(const CounterDescription &desc, const CounterResult &result)
{
  if(desc.resultType == CompType::UInt)
  {
    if(desc.resultByteWidth <= 4)
      return QString::number(uint64_t(result.value.u32));

    return QString::number(result.value.u64);
  }

  if(desc.resultByteWidth <= 4)
    return result.value.f;

  return result.value.d;
}

bool IsPerformanceRelevantAction(const ExportActionInfo &action)
{
  if(action.fakeMarker || action.eventId == 0)
    return false;

  return bool(action.flags &
              (ActionFlags::Drawcall | ActionFlags::Dispatch | ActionFlags::MeshDispatch |
               ActionFlags::DispatchRay | ActionFlags::Copy | ActionFlags::Resolve |
               ActionFlags::Clear | ActionFlags::Present));
}

bool ActionUsesPipelineState(const ExportActionInfo &action)
{
  return bool(action.flags &
              (ActionFlags::Drawcall | ActionFlags::Dispatch | ActionFlags::MeshDispatch |
               ActionFlags::DispatchRay));
}

QString FormatMilliseconds(double milliseconds)
{
  if(milliseconds < 0.0)
    return lit("-");

  return QString::number(milliseconds, 'f', 3);
}

void CollectExportActions(const rdcarray<ActionDescription> &actions, const SDFile &structuredFile,
                          const QStringList &parents, QVector<ExportActionInfo> &out)
{
  for(const ActionDescription &action : actions)
  {
    ExportActionInfo info;
    info.actionId = action.actionId;
    info.eventId = action.eventId;
    info.name = DisplayActionName(action, structuredFile);
    info.hierarchy = parents.join(lit(" / "));
    info.kind = ActionKind(action.flags);
    info.flags = action.flags;
    info.flagsText = ToQStr(action.flags);
    info.fakeMarker = action.IsFakeMarker();
    info.numIndices = action.numIndices;
    info.numInstances = action.numInstances;
    info.baseVertex = action.baseVertex;
    info.indexOffset = action.indexOffset;
    info.vertexOffset = action.vertexOffset;
    info.instanceOffset = action.instanceOffset;
    info.drawIndex = action.drawIndex;
    info.dispatchDimension = action.dispatchDimension;
    info.dispatchThreadsDimension = action.dispatchThreadsDimension;
    info.dispatchBase = action.dispatchBase;
    info.copySource = action.copySource;
    info.copyDestination = action.copyDestination;
    info.depthOut = action.depthOut;
    info.outputs = action.outputs;
    info.eventIds.reserve(action.events.count());

    for(const APIEvent &event : action.events)
      info.eventIds.push_back(event.eventId);

    if(info.eventIds.isEmpty() && info.eventId != 0)
      info.eventIds.push_back(info.eventId);

    out.push_back(info);

    QStringList childParents = parents;
    if(!action.children.empty())
      childParents.push_back(info.name);

    CollectExportActions(action.children, structuredFile, childParents, out);
  }
}
}    // namespace

static void ExportPerformanceSummary(MainWindow *window, ICaptureContext &ctx,
                                     const QString &outputRoot)
{
  QString exportTimestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
  const FrameDescription &frameInfo = ctx.FrameInfo();
  const FrameStatistics frameStats = frameInfo.stats;
  uint32_t frameNumber = frameInfo.frameNumber;
  bool hasFrameNumber = frameNumber != FrameDescription::NoFrameNumber;
  QString captureFilename = ctx.GetCaptureFilename();
  QString driverName = FromRDCStr(ctx.Replay().GetCaptureAccess()->DriverName());
  GraphicsAPI apiType = ctx.APIProps().pipelineType;
  QString apiName = ToQStr(apiType);
  QJsonObject frameStatsJson = MakeFrameStatsJson(frameStats);

  QMap<ResourceId, ResourceDescription> resourceDescs;
  QMap<ResourceId, TextureDescription> textureDescs;
  QMap<ResourceId, BufferDescription> bufferDescs;
  QMap<ResourceId, QString> resourceNames;

  for(const ResourceDescription &resource : ctx.GetResources())
  {
    ResourceDescription copy = resource;
    copy.annotations = NULL;
    resourceDescs.insert(copy.resourceId, copy);
    resourceNames.insert(copy.resourceId, ctx.GetResourceName(copy.resourceId));
  }

  for(const TextureDescription &texture : ctx.GetTextures())
    textureDescs.insert(texture.resourceId, texture);

  for(const BufferDescription &buffer : ctx.GetBuffers())
    bufferDescs.insert(buffer.resourceId, buffer);

  QVector<ExportActionInfo> allActions;
  CollectExportActions(ctx.CurRootActions(), ctx.GetStructuredFile(), {}, allActions);

  QVector<ExportActionInfo> actions;
  actions.reserve(allActions.count());
  for(const ExportActionInfo &action : allActions)
  {
    if(IsPerformanceRelevantAction(action))
      actions.push_back(action);
  }

  const int filteredOutActionCount = allActions.count() - actions.count();

  QVector<DebugMessage> frameLevelPerformanceMessages;
  QJsonArray performanceMessagesJson;
  QMap<uint32_t, int> performanceMessageCountByEvent;
  int totalPerformanceMessageCount = 0;

  for(const DebugMessage &message : ctx.DebugMessages())
  {
    if(message.category != MessageCategory::Performance)
      continue;

    performanceMessagesJson.append(MakeDebugMessageJson(message));
    performanceMessageCountByEvent[message.eventId] =
        performanceMessageCountByEvent.value(message.eventId) + 1;

    if(message.eventId == 0)
      frameLevelPerformanceMessages.push_back(message);

    totalPerformanceMessageCount++;
  }

  const QVector<GPUCounter> desiredCounters = {
      GPUCounter::EventGPUDuration,      GPUCounter::InputVerticesRead,
      GPUCounter::IAPrimitives,          GPUCounter::GSPrimitives,
      GPUCounter::RasterizerInvocations, GPUCounter::RasterizedPrimitives,
      GPUCounter::SamplesPassed,         GPUCounter::VSInvocations,
      GPUCounter::HSInvocations,         GPUCounter::DSInvocations,
      GPUCounter::GSInvocations,         GPUCounter::PSInvocations,
      GPUCounter::CSInvocations,         GPUCounter::ASInvocations,
      GPUCounter::MSInvocations,
  };

  QJsonArray requestedCounterNamesJson;
  for(GPUCounter counter : desiredCounters)
    requestedCounterNamesJson.append(ToQStr(counter));

  uint32_t originalSelectedEvent = ctx.CurSelectedEvent();
  uint32_t originalEvent = ctx.CurEvent();

  std::atomic<bool> finished(false);
  std::atomic<float> progress(0.0f);
  std::atomic<bool> cancelRequested(false);
  bool cancelledByUser = false;
  QString fatalError;
  QJsonArray actionDocs;
  QJsonArray hotspotDocs;
  QJsonArray selectedCounterDescriptionsJson;
  QJsonArray missingCounterNamesJson;
  int actionsWithCounterData = 0;
  int actionsWithGpuDuration = 0;
  int actionsWithPerformanceMessages = 0;
  QVector<ExportActionSummary> actionSummaries;

  LambdaThread *exportThread = new LambdaThread(
      [&ctx, outputRoot, exportTimestamp, captureFilename, driverName, apiName, apiType,
       frameNumber, hasFrameNumber, frameStatsJson, actions, filteredOutActionCount,
       desiredCounters, requestedCounterNamesJson, performanceMessagesJson,
       frameLevelPerformanceMessages, performanceMessageCountByEvent, totalPerformanceMessageCount,
       resourceDescs, textureDescs, bufferDescs, resourceNames, &finished, &progress,
       &cancelRequested, &cancelledByUser, &fatalError, &actionDocs, &hotspotDocs,
       &selectedCounterDescriptionsJson, &missingCounterNamesJson, &actionsWithCounterData,
       &actionsWithGpuDuration, &actionsWithPerformanceMessages, &actionSummaries]() mutable {
        auto finish = [&]() { finished.store(true); };
        auto setProgress = [&](int completed, int total) {
          progress.store(total > 0 ? float(completed) / float(total) : 1.0f);
        };

        int totalSteps = qMax(1, actions.count() + 2);
        int completedSteps = 0;

        QDir rootDir(outputRoot);

        if(!QDir().mkpath(rootDir.absolutePath()))
        {
          fatalError = QFormatStr("Couldn't create export directory '%1'.").arg(outputRoot);
          finish();
          return;
        }

        completedSteps++;
        setProgress(completedSteps, totalSteps);

        QMap<uint32_t, CounterDescription> counterDescriptionsById;
        QMap<uint32_t, QMap<uint32_t, QJsonValue>> counterValuesByEvent;
        QMap<uint32_t, double> gpuDurationMsByEvent;

        ctx.Replay().BlockInvoke([&](IReplayController *r) {
          if(cancelRequested.load())
          {
            cancelledByUser = true;
            return;
          }

          rdcarray<GPUCounter> availableCounters = r->EnumerateCounters();

          auto counterAvailable = [&](GPUCounter target) {
            for(GPUCounter available : availableCounters)
            {
              if(available == target)
                return true;
            }

            return false;
          };

          QVector<GPUCounter> selectedCounters;
          selectedCounters.reserve(desiredCounters.count());

          for(GPUCounter counter : desiredCounters)
          {
            if(counterAvailable(counter))
              selectedCounters.push_back(counter);
            else
              missingCounterNamesJson.append(ToQStr(counter));
          }

          for(GPUCounter counter : selectedCounters)
          {
            CounterDescription desc = r->DescribeCounter(counter);
            counterDescriptionsById[uint32_t(counter)] = desc;
            selectedCounterDescriptionsJson.append(MakeCounterDescriptionJson(desc));
          }

          if(!selectedCounters.isEmpty())
          {
            rdcarray<GPUCounter> fetchCounters;
            fetchCounters.resize(selectedCounters.count());

            for(int i = 0; i < selectedCounters.count(); i++)
              fetchCounters[i] = selectedCounters[i];

            rdcarray<CounterResult> results = r->FetchCounters(fetchCounters);

            for(const CounterResult &result : results)
            {
              uint32_t counterId = uint32_t(result.counter);

              if(!counterDescriptionsById.contains(counterId))
                continue;

              const CounterDescription &desc = counterDescriptionsById[counterId];

              if(desc.counter == GPUCounter::EventGPUDuration)
              {
                double durationSeconds =
                    desc.resultByteWidth <= 4 ? double(result.value.f) : result.value.d;

                if(durationSeconds >= 0.0)
                  gpuDurationMsByEvent[result.eventId] = durationSeconds * 1000.0;
                else
                  continue;
              }

              counterValuesByEvent[result.eventId][counterId] = MakeCounterValueJson(desc, result);
            }
          }

          completedSteps++;
          setProgress(completedSteps, totalSteps);

          for(const ExportActionInfo &action : actions)
          {
            if(cancelRequested.load())
            {
              cancelledByUser = true;
              return;
            }

            QJsonObject actionDoc;
            actionDoc[lit("action_id")] = int(action.actionId);
            actionDoc[lit("event_id")] = int(action.eventId);
            actionDoc[lit("name")] = action.name;
            actionDoc[lit("hierarchy")] =
                action.hierarchy.isEmpty() ? lit("<root>") : action.hierarchy;
            actionDoc[lit("kind")] = action.kind;
            actionDoc[lit("flags")] = action.flagsText;
            actionDoc[lit("event_ids")] = ToJsonArray(action.eventIds);

            if(action.flags & (ActionFlags::Drawcall | ActionFlags::MeshDispatch))
            {
              actionDoc[lit("num_indices")] = int(action.numIndices);
              actionDoc[lit("num_instances")] = int(action.numInstances);
              actionDoc[lit("base_vertex")] = action.baseVertex;
              actionDoc[lit("index_offset")] = int(action.indexOffset);
              actionDoc[lit("vertex_offset")] = int(action.vertexOffset);
              actionDoc[lit("instance_offset")] = int(action.instanceOffset);
              actionDoc[lit("draw_index")] = int(action.drawIndex);
            }

            if(action.flags &
               (ActionFlags::Dispatch | ActionFlags::MeshDispatch | ActionFlags::DispatchRay))
            {
              actionDoc[lit("dispatch_dimension")] = ToJsonArray(action.dispatchDimension);
              actionDoc[lit("dispatch_threads_dimension")] =
                  ToJsonArray(action.dispatchThreadsDimension);
              actionDoc[lit("dispatch_base")] = ToJsonArray(action.dispatchBase);
            }

            const QMap<uint32_t, QJsonValue> eventCounterValues =
                counterValuesByEvent.value(action.eventId);
            QJsonObject counterValuesJson;

            for(auto it = eventCounterValues.begin(); it != eventCounterValues.end(); ++it)
            {
              if(!counterDescriptionsById.contains(it.key()))
                continue;

              const CounterDescription &desc = counterDescriptionsById[it.key()];
              counterValuesJson[ToQStr(desc.counter)] = it.value();
            }

            const int counterValueCount = eventCounterValues.size();
            if(!counterValuesJson.isEmpty())
            {
              actionDoc[lit("counters")] = counterValuesJson;
              actionsWithCounterData++;
            }

            double gpuDurationMs = -1.0;
            if(gpuDurationMsByEvent.contains(action.eventId))
            {
              gpuDurationMs = gpuDurationMsByEvent[action.eventId];
              actionDoc[lit("gpu_duration_ms")] = gpuDurationMs;
              actionsWithGpuDuration++;
            }

            int performanceMessageCount = 0;
            for(uint32_t eventId : action.eventIds)
              performanceMessageCount += performanceMessageCountByEvent.value(eventId);

            actionDoc[lit("performance_message_count")] = performanceMessageCount;
            if(performanceMessageCount > 0)
              actionsWithPerformanceMessages++;

            QJsonArray relatedResourcesJson;

            auto appendResource = [&](const QString &label, ResourceId id) {
              QJsonObject resourceJson = MakeResourceSummaryJson(
                  label, id, resourceNames, resourceDescs, textureDescs, bufferDescs);
              if(!resourceJson.isEmpty())
                relatedResourcesJson.append(resourceJson);
            };

            appendResource(lit("copy_source"), action.copySource);
            appendResource(lit("copy_destination"), action.copyDestination);
            appendResource(lit("depth_output"), action.depthOut);

            for(size_t i = 0; i < action.outputs.size(); i++)
            {
              if(action.outputs[i] != ResourceId())
                appendResource(QFormatStr("color_output_%1").arg(int(i)), action.outputs[i]);
            }

            if(!relatedResourcesJson.isEmpty())
              actionDoc[lit("related_resources")] = relatedResourcesJson;

            if(ActionUsesPipelineState(action))
            {
              r->SetFrameEvent(action.eventId, false);
              const PipeState &pipe = r->GetPipelineState();

              ResourceId graphicsPipeline = pipe.GetGraphicsPipelineObject();
              ResourceId computePipeline = pipe.GetComputePipelineObject();

              if(graphicsPipeline != ResourceId())
              {
                actionDoc[lit("graphics_pipeline_object")] = ResourceIdText(graphicsPipeline);

                QString pipelineName = resourceNames.value(graphicsPipeline).trimmed();
                if(!pipelineName.isEmpty())
                  actionDoc[lit("graphics_pipeline_name")] = pipelineName;
              }

              if(computePipeline != ResourceId())
              {
                actionDoc[lit("compute_pipeline_object")] = ResourceIdText(computePipeline);

                QString pipelineName = resourceNames.value(computePipeline).trimmed();
                if(!pipelineName.isEmpty())
                  actionDoc[lit("compute_pipeline_name")] = pipelineName;
              }

              QJsonArray shaderBindingsJson;
              QJsonArray bindingCountsJson;

              for(ShaderStage stage : {ShaderStage::Vertex, ShaderStage::Hull,
                                       ShaderStage::Domain, ShaderStage::Geometry,
                                       ShaderStage::Pixel, ShaderStage::Compute,
                                       ShaderStage::Task, ShaderStage::Mesh})
              {
                QString stageName = ToQStr(stage, apiType);
                ResourceId shaderId = pipe.GetShader(stage);
                int constantCount = pipe.GetConstantBlocks(stage, true).count();
                int readOnlyCount = pipe.GetReadOnlyResources(stage, true).count();
                int readWriteCount = pipe.GetReadWriteResources(stage, true).count();
                int samplerCount = pipe.GetSamplers(stage, true).count();

                if(shaderId == ResourceId() && constantCount == 0 && readOnlyCount == 0 &&
                   readWriteCount == 0 && samplerCount == 0)
                {
                  continue;
                }

                if(shaderId != ResourceId())
                {
                  QJsonObject shaderJson;
                  shaderJson[lit("stage")] = stageName;
                  shaderJson[lit("resource_id")] = ResourceIdText(shaderId);

                  QString shaderName = resourceNames.value(shaderId).trimmed();
                  shaderJson[lit("name")] =
                      shaderName.isEmpty() ? ResourceIdText(shaderId) : shaderName;
                  shaderBindingsJson.append(shaderJson);
                }

                QJsonObject bindingJson;
                bindingJson[lit("stage")] = stageName;
                bindingJson[lit("constant_buffers")] = constantCount;
                bindingJson[lit("read_only_resources")] = readOnlyCount;
                bindingJson[lit("read_write_resources")] = readWriteCount;
                bindingJson[lit("samplers")] = samplerCount;
                bindingCountsJson.append(bindingJson);
              }

              if(!shaderBindingsJson.isEmpty())
                actionDoc[lit("shader_bindings")] = shaderBindingsJson;
              if(!bindingCountsJson.isEmpty())
                actionDoc[lit("binding_counts")] = bindingCountsJson;
            }

            actionDocs.append(actionDoc);

            ExportActionSummary summary;
            summary.action = action;
            summary.gpuDurationMs = gpuDurationMs;
            summary.counterValueCount = counterValueCount;
            summary.performanceMessageCount = performanceMessageCount;
            actionSummaries.push_back(summary);

            completedSteps++;
            setProgress(completedSteps, totalSteps);
          }
        });

        auto hotspotSort = [](const ExportActionSummary &a, const ExportActionSummary &b) {
          const bool aHasDuration = a.gpuDurationMs >= 0.0;
          const bool bHasDuration = b.gpuDurationMs >= 0.0;

          if(aHasDuration != bHasDuration)
            return aHasDuration > bHasDuration;

          if(aHasDuration && a.gpuDurationMs != b.gpuDurationMs)
            return a.gpuDurationMs > b.gpuDurationMs;

          if(a.performanceMessageCount != b.performanceMessageCount)
            return a.performanceMessageCount > b.performanceMessageCount;

          return a.action.eventId < b.action.eventId;
        };

        QVector<ExportActionSummary> hotspotSummaries = actionSummaries;
        std::sort(hotspotSummaries.begin(), hotspotSummaries.end(), hotspotSort);

        for(const ExportActionSummary &summary : hotspotSummaries)
        {
          if(summary.gpuDurationMs < 0.0)
            continue;

          QJsonObject hotspot;
          hotspot[lit("action_id")] = int(summary.action.actionId);
          hotspot[lit("event_id")] = int(summary.action.eventId);
          hotspot[lit("name")] = summary.action.name;
          hotspot[lit("kind")] = summary.action.kind;
          hotspot[lit("hierarchy")] =
              summary.action.hierarchy.isEmpty() ? lit("<root>") : summary.action.hierarchy;
          hotspot[lit("gpu_duration_ms")] = summary.gpuDurationMs;
          hotspot[lit("performance_message_count")] = summary.performanceMessageCount;
          hotspotDocs.append(hotspot);

          if(hotspotDocs.count() >= MaxHotspotCount)
            break;
        }

        if(fatalError.isEmpty())
        {
          QJsonObject manifest;
          manifest[lit("export_type")] = lit("performance_summary");
          manifest[lit("capture_file")] = captureFilename;
          manifest[lit("driver")] = driverName;
          manifest[lit("graphics_api")] = apiName;
          manifest[lit("generated_at")] = exportTimestamp;
          manifest[lit("status")] = cancelledByUser ? lit("cancelled") : lit("completed");
          if(hasFrameNumber)
            manifest[lit("frame_number")] = int(frameNumber);
          manifest[lit("frame_stats_recorded")] = frameStatsJson[lit("recorded")].toBool();
          manifest[lit("total_relevant_actions")] = actions.count();
          manifest[lit("exported_action_count")] = actionDocs.count();
          manifest[lit("filtered_out_action_count")] = filteredOutActionCount;
          manifest[lit("captured_counter_count")] = selectedCounterDescriptionsJson.count();
          manifest[lit("missing_counter_count")] = missingCounterNamesJson.count();
          manifest[lit("actions_with_counter_data")] = actionsWithCounterData;
          manifest[lit("actions_with_gpu_duration")] = actionsWithGpuDuration;
          manifest[lit("actions_with_performance_messages")] = actionsWithPerformanceMessages;
          manifest[lit("performance_message_count")] = totalPerformanceMessageCount;
          manifest[lit("frame_level_performance_message_count")] =
              frameLevelPerformanceMessages.count();

          QJsonObject files;
          files[lit("frame_stats")] = lit("frame_stats.json");
          files[lit("counters")] = lit("counters.json");
          files[lit("actions")] = lit("actions.json");
          files[lit("hotspots")] = lit("hotspots.json");
          files[lit("performance_messages")] = lit("performance_messages.json");
          files[lit("index")] = lit("index.md");
          manifest[lit("files")] = files;

          QJsonObject countersDoc;
          countersDoc[lit("requested_generic_counters")] = requestedCounterNamesJson;
          countersDoc[lit("captured_counter_count")] = selectedCounterDescriptionsJson.count();
          countersDoc[lit("missing_counter_count")] = missingCounterNamesJson.count();
          countersDoc[lit("counters")] = selectedCounterDescriptionsJson;
          countersDoc[lit("missing_counters")] = missingCounterNamesJson;

          QJsonObject actionsDocRoot;
          actionsDocRoot[lit("count")] = actionDocs.count();
          actionsDocRoot[lit("actions")] = actionDocs;

          QJsonObject hotspotsDocRoot;
          hotspotsDocRoot[lit("count")] = hotspotDocs.count();
          hotspotsDocRoot[lit("hotspots")] = hotspotDocs;

          QJsonObject messagesDocRoot;
          messagesDocRoot[lit("count")] = performanceMessagesJson.count();
          messagesDocRoot[lit("messages")] = performanceMessagesJson;

          QString writeError;
          if(!WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("manifest.json")), manifest,
                            writeError) ||
             !WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("frame_stats.json")),
                            frameStatsJson, writeError) ||
             !WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("counters.json")), countersDoc,
                            writeError) ||
             !WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("actions.json")), actionsDocRoot,
                            writeError) ||
             !WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("hotspots.json")),
                            hotspotsDocRoot, writeError) ||
             !WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("performance_messages.json")),
                            messagesDocRoot, writeError))
          {
            fatalError = writeError;
          }
        }

        progress.store(1.0f);
        finish();
      });

  exportThread->setName(lit("Export Performance Summary"));
  exportThread->start();

  ShowProgressDialog(window, window->tr("Exporting performance summary, please wait..."),
                     [&finished]() { return finished.load(); },
                     [&progress]() { return progress.load(); },
                     [&cancelRequested]() { cancelRequested.store(true); });

  exportThread->wait();
  exportThread->deleteLater();

  ctx.SetEventID({}, originalSelectedEvent, originalEvent, true);

  if(!fatalError.isEmpty())
  {
    RDDialog::critical(
        window, window->tr("Export Performance Summary Failed"),
        window->tr("Performance summary export failed. Partial output may exist in:\n%1\n\n%2")
            .arg(outputRoot)
            .arg(fatalError));
  }
  else if(cancelledByUser)
  {
    RDDialog::information(
        window, window->tr("Export Performance Summary Cancelled"),
        window->tr("Performance summary export was cancelled. Partial output was written to:\n%1")
            .arg(outputRoot));
  }
  else
  {
    QString index;
    QTextStream indexStream(&index);
    indexStream << "# RenderDoc Performance Export\n\n";
    indexStream << "- Capture file: `" << captureFilename << "`\n";
    indexStream << "- Driver: `" << driverName << "`\n";
    indexStream << "- Graphics API: `" << apiName << "`\n";
    if(hasFrameNumber)
      indexStream << "- Frame number: `" << frameNumber << "`\n";
    indexStream << "- Generated at: `" << exportTimestamp << "`\n";
    indexStream << "- Total performance-relevant actions: `" << actions.count() << "`\n";
    indexStream << "- Filtered-out non-performance actions: `" << filteredOutActionCount
                << "`\n";
    indexStream << "- Captured counters: `" << selectedCounterDescriptionsJson.count() << "`\n";
    indexStream << "- Missing requested counters: `" << missingCounterNamesJson.count() << "`\n";
    indexStream << "- Actions with GPU duration: `" << actionsWithGpuDuration << "`\n";
    indexStream << "- Actions with performance messages: `" << actionsWithPerformanceMessages
                << "`\n";
    indexStream << "- Performance messages: `" << totalPerformanceMessageCount << "`\n\n";

    indexStream << "## Top GPU Hotspots\n\n";

    QVector<ExportActionSummary> hotspotSummaries = actionSummaries;
    std::sort(hotspotSummaries.begin(), hotspotSummaries.end(),
              [](const ExportActionSummary &a, const ExportActionSummary &b) {
                const bool aHasDuration = a.gpuDurationMs >= 0.0;
                const bool bHasDuration = b.gpuDurationMs >= 0.0;

                if(aHasDuration != bHasDuration)
                  return aHasDuration > bHasDuration;

                if(aHasDuration && a.gpuDurationMs != b.gpuDurationMs)
                  return a.gpuDurationMs > b.gpuDurationMs;

                return a.action.eventId < b.action.eventId;
              });

    int hotspotLineCount = 0;
    for(const ExportActionSummary &summary : hotspotSummaries)
    {
      if(summary.gpuDurationMs < 0.0)
        continue;

      indexStream << "- `E" << summary.action.eventId << "` `" << summary.action.kind << "` "
                  << summary.action.name << " - `" << FormatMilliseconds(summary.gpuDurationMs)
                  << " ms`";

      if(summary.performanceMessageCount > 0)
        indexStream << " - perf messages `" << summary.performanceMessageCount << "`";

      if(!summary.action.hierarchy.isEmpty())
        indexStream << " - hierarchy `" << summary.action.hierarchy << "`";

      indexStream << "\n";

      hotspotLineCount++;
      if(hotspotLineCount >= 25)
        break;
    }

    if(hotspotLineCount == 0)
      indexStream << "- No GPU duration data was available.\n";

    indexStream << "\n## Frame-Level Performance Messages\n\n";

    if(frameLevelPerformanceMessages.isEmpty())
    {
      indexStream << "- None\n";
    }
    else
    {
      for(const DebugMessage &message : frameLevelPerformanceMessages)
      {
        indexStream << "- `" << ToQStr(message.severity) << "` `" << ToQStr(message.source)
                    << "`: " << FromRDCStr(message.description) << "\n";
      }
    }

    QString writeError;
    if(!WriteTextFile(QDir(outputRoot).absoluteFilePath(lit("index.md")), index, writeError))
    {
      RDDialog::critical(
          window, window->tr("Export Performance Summary Failed"),
          window->tr("Performance summary export completed, but writing `index.md` failed.\n\n%1")
              .arg(writeError));
      return;
    }

    RDDialog::information(
        window, window->tr("Export Performance Summary Complete"),
        window->tr("Performance summary export completed successfully.\n\nOutput:\n%1")
            .arg(outputRoot));
  }
}

NetworkWorker::NetworkWorker() : QObject(NULL)
{
}

NetworkWorker::~NetworkWorker()
{
}

void NetworkWorker::get(QUrl url)
{
  if(manager == NULL)
    manager = new QNetworkAccessManager(this);

  // create the request
  QNetworkReply *req = manager->get(QNetworkRequest(url));

  // connect up error and finished slots on *this* thread, and in the lambda emit signals to
  // cross-thread back onto the UI thread.
  QObject::connect(req, OverloadedSlot<QNetworkReply::NetworkError>::of(&QNetworkReply::error),
                   [this, req](QNetworkReply::NetworkError) {
                     emit requestFailed(req->url(), req->errorString());
                   });

  QObject::connect(req, &QNetworkReply::finished, [this, req]() {
    if(req->error() != QNetworkReply::NoError)
    {
      emit requestFailed(req->url(), req->errorString());
      return;
    }

    QByteArray replyData = req->readAll();

    emit requestCompleted(req->url(), replyData);
  });
}

void MainWindow::MakeNetworkRequest(QUrl url, std::function<void(QByteArray)> success,
                                    std::function<void(QString)> failure)
{
  m_NetworkCompleteCallbacks[url] = success;
  if(failure)
    m_NetworkFailCallbacks[url] = failure;

  // fire over onto the network thread
  emit networkRequestGet(url);
}

MainWindow::MainWindow(ICaptureContext &ctx) : QMainWindow(NULL), ui(new Ui::MainWindow), m_Ctx(ctx)
{
  ui->setupUi(this);

  setProperty("ICaptureContext", QVariant::fromValue((void *)&ctx));

#if defined(Q_OS_WIN32)
  // remove inject menu item when it's not enabled in the settings
  if(!ctx.Config().AllowProcessInject)
    ui->menu_File->removeAction(ui->action_Inject_into_Process);
#else
  // process injection is not supported on non-Windows, so remove the menu item rather than disable
  // it without a clear way to communicate that it is never supported
  ui->menu_File->removeAction(ui->action_Inject_into_Process);
#endif

  QToolTip::setPalette(palette());

  qApp->installEventFilter(this);

  setAcceptDrops(true);

  QObject::connect(ui->menu_Tools, &QMenu::aboutToShow, this, &MainWindow::updateToolsMenuOptions);

  QObject::connect(ui->action_Load_Default_Layout, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);
  QObject::connect(ui->action_Load_Layout_1, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);
  QObject::connect(ui->action_Load_Layout_2, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);
  QObject::connect(ui->action_Load_Layout_3, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);
  QObject::connect(ui->action_Load_Layout_4, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);
  QObject::connect(ui->action_Load_Layout_5, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);
  QObject::connect(ui->action_Load_Layout_6, &QAction::triggered, this,
                   &MainWindow::loadLayout_triggered);

  QObject::connect(ui->action_Save_Default_Layout, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);
  QObject::connect(ui->action_Save_Layout_1, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);
  QObject::connect(ui->action_Save_Layout_2, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);
  QObject::connect(ui->action_Save_Layout_3, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);
  QObject::connect(ui->action_Save_Layout_4, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);
  QObject::connect(ui->action_Save_Layout_5, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);
  QObject::connect(ui->action_Save_Layout_6, &QAction::triggered, this,
                   &MainWindow::saveLayout_triggered);

  QObject::connect(ui->action_Launch_Application_Window, &QAction::triggered, this,
                   &MainWindow::on_action_Launch_Application_triggered);

  QObject::connect(ui->action_Clear_Capture_Files_History, &QAction::triggered, this,
                   &MainWindow::ClearRecentCaptureFiles);
  QObject::connect(ui->action_Clear_Capture_Settings_History, &QAction::triggered, this,
                   &MainWindow::ClearRecentCaptureSettings);

  contextChooserMenu = new RDMenu(this);

  FillRemotesMenu(contextChooserMenu, true);

  contextChooser = new QToolButton(this);
  contextChooser->setText(tr("Replay Context: %1").arg(tr("Local")));
  contextChooser->setIcon(Icons::house());
  contextChooser->setAutoRaise(true);
  contextChooser->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  contextChooser->setPopupMode(QToolButton::InstantPopup);
  contextChooser->setMenu(contextChooserMenu);
  contextChooser->setContextMenuPolicy(Qt::DefaultContextMenu);
  QObject::connect(contextChooserMenu, &QMenu::aboutToShow, this,
                   &MainWindow::contextChooser_menuShowing);
  QObject::connect(contextChooserMenu, &RDMenu::keyPress, [this](QKeyEvent *ev) {
    QList<QAction *> actions = contextChooserMenu->actions();
    if(ev->key() == Qt::Key_L)
    {
      actions.last()->trigger();
      contextChooserMenu->close();
    }
    else if(ev->key() >= Qt::Key_1 && ev->key() <= Qt::Key_9)
    {
      int idx = ev->key() - Qt::Key_1;
      if(idx < actions.size())
        actions[idx]->trigger();
      contextChooserMenu->close();
    }
  });

  ui->statusBar->addWidget(contextChooser);

  statusIcon = new RDLabel(this);
  ui->statusBar->addWidget(statusIcon);

  statusText = new RDLabel(this);
  ui->statusBar->addWidget(statusText);

  statusProgress = new QProgressBar(this);
  ui->statusBar->addWidget(statusProgress);

  statusProgress->setVisible(false);
  statusProgress->setMinimumSize(QSize(200, 0));
  statusProgress->setMinimum(0);
  statusProgress->setMaximum(1000);

  statusIcon->setText(QString());
  statusIcon->setPixmap(QPixmap());
  statusText->setText(QString());

  QObject::connect(statusIcon, &RDLabel::doubleClicked, this, &MainWindow::statusDoubleClicked);
  QObject::connect(statusText, &RDLabel::doubleClicked, this, &MainWindow::statusDoubleClicked);

  QObject::connect(&m_MessageTick, &QTimer::timeout, this, &MainWindow::messageCheck);
  m_MessageTick.setSingleShot(false);
  m_MessageTick.setInterval(175);
  m_MessageTick.start();

  QTimer *vkconfigCheckTimer = new QTimer(this);
  QObject::connect(vkconfigCheckTimer, &QTimer::timeout, [vkconfigCheckTimer]() {
    QString homePath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);

    // for some reason these paths have changed a lot so we have to check them all :(
    const QString basePaths[] = {
#if defined(Q_OS_WIN32)
      lit("/AppData/Local/LunarG/vkconfig/override/"),
      lit("/AppData/Local/LunarG/vulkan/"),
#else
      lit("/.local/share/vulkan/implicit_layer.d/"),
      lit("/.local/share/vulkan/loader_settings.d/"),
#endif
    };

    const QString filenames[] = {
        lit("VkLayerOverride.json"),
        lit("VkLayer_Override.json"),
        lit("VkLayer_override.json"),
        lit("vk_loader_settings.json"),
    };

    for(const QString &path : basePaths)
    {
      for(const QString &fn : filenames)
      {
        QFileInfo vkconfigcheck(homePath + path + fn);

        if(vkconfigcheck.exists() && vkconfigcheck.isFile())
        {
          RDDialog::warning(
              NULL, tr("vkconfig detected - possible incompatibility"),
              tr("Configuration from 'vkconfig' tool detected.\n\n"
                 "This program has caused problems in the past and it is \n"
                 "strongly recommended that you disable it while using RenderDoc.\n\n"
                 "If this program is not active check the path below for any leftover files:\n\n%1")
                  .arg(vkconfigcheck.absoluteFilePath()));

          qInfo() << "vkconfig detected and warned";
          vkconfigCheckTimer->stop();
          return;
        }
      }
    }
  });

  vkconfigCheckTimer->setSingleShot(false);
  vkconfigCheckTimer->setInterval(2500);
  vkconfigCheckTimer->start();

  m_RemoteProbeSemaphore.release();
  m_RemoteProbe = new LambdaThread([this]() {
    // fetch all device protocols to start them processing
    rdcarray<rdcstr> protocols;
    RENDERDOC_GetSupportedDeviceProtocols(&protocols);
    for(const rdcstr &p : protocols)
      RENDERDOC_GetDeviceProtocolController(p);

    while(m_RemoteProbeSemaphore.available())
    {
      // do a remoteProbe immediately to populate the device list on startup.
      remoteProbe();

      // allow any early-init replay host switches now that we've populated the device list
      m_RemoteInitialProbeReady.release();

      // do several small sleeps so we can respond quicker when we need to shut down
      for(int i = 0; i < 50; i++)
      {
        QThread::msleep(150);
        if(!m_RemoteProbeSemaphore.available())
          return;
      }
    }
  });
  m_RemoteProbe->setName(lit("Remote Probe"));
  m_RemoteProbe->start();

  SetTitle();

#if defined(RELEASE)
  ui->action_Send_Error_Report->setEnabled(true);
#else
  ui->action_Send_Error_Report->setEnabled(false);
#endif

  // only allow sending error reports if we have a valid git commit hash
  rdcstr hash = RENDERDOC_GetCommitHash();
  if(hash.length() != 40 || hash.find_first_not_of("0123456789abcdef") >= 0)
  {
    qInfo() << "Disabling error reports due to invalid commit hash";
    ui->action_Send_Error_Report->setEnabled(false);
  }

  m_NetWorker = new NetworkWorker;
  m_NetManagerThread = new LambdaThread([this]() {
    QEventLoop loop;
    loop.exec();
    delete m_NetWorker;
  });
  m_NetManagerThread->moveObjectToThread(m_NetWorker);
  m_NetManagerThread->start();
  m_NetManagerThread->thread()->setPriority(QThread::LowPriority);

  // set up cross-thread signal/slot connections
  QObject::connect(this, &MainWindow::networkRequestGet, m_NetWorker, &NetworkWorker::get,
                   Qt::QueuedConnection);
  QObject::connect(m_NetWorker, &NetworkWorker::requestFailed, this,
                   &MainWindow::networkRequestFailed, Qt::QueuedConnection);
  QObject::connect(m_NetWorker, &NetworkWorker::requestCompleted, this,
                   &MainWindow::networkRequestCompleted, Qt::QueuedConnection);

  updateAction = new QAction(this);
  updateAction->setText(tr("Update Available!"));
  updateAction->setIcon(Icons::update());

  QObject::connect(updateAction, &QAction::triggered, this, &MainWindow::updateAvailable_triggered);

#if !defined(Q_OS_WIN32)
  // update checks only happen on windows
  {
    QList<QAction *> actions = ui->menu_Help->actions();
    int idx = actions.indexOf(ui->action_Check_for_Updates);
    idx++;
    if(idx < actions.count() && actions[idx]->isSeparator())
      delete actions[idx];

    delete ui->action_Check_for_Updates;
    ui->action_Check_for_Updates = NULL;

    delete updateAction;
    updateAction = NULL;
  }
#endif

  if(updateAction)
  {
    ui->menuBar->addAction(updateAction);
    updateAction->setVisible(false);
  }

  PopulateRecentCaptureFiles();
  PopulateRecentCaptureSettings();
  PopulateReportedBugs();

  CheckUpdates();

  rdcarray<BugReport> bugs = m_Ctx.Config().CrashReport_ReportedBugs;
  LambdaThread *bugupdate = new LambdaThread([this, bugs]() {
    QDateTime now = QDateTime::currentDateTimeUtc();

    // loop over all the bugs
    for(const BugReport &b : bugs)
    {
      // check bugs every two days
      qint64 diff = QDateTime(b.checkDate).secsTo(now);
      if(diff > 2 * 24 * 60 * 60)
      {
        // update the check date on the stored bug
        GUIInvoke::call(this, [this, b, now]() {
          for(BugReport &bug : m_Ctx.Config().CrashReport_ReportedBugs)
          {
            if(bug.reportId == b.reportId)
            {
              bug.checkDate = now;
              break;
            }
          }
          m_Ctx.Config().Save();

          // call out to the status-check to see when the bug report was last updated
          MakeNetworkRequest(QUrl(QString(b.URL()) + lit("/check")), [this, b](QByteArray replyData) {
            QString response = QString::fromUtf8(replyData);

            if(response.isEmpty())
              return;

            // only look at the first line of the response
            int idx = response.indexOf(QLatin1Char('\n'));

            if(idx > 0)
              response.truncate(idx);

            QDateTime update = QDateTime::fromString(response, lit("yyyy-MM-dd HH:mm:ss"));

            // if there's been an update since the last check, set unread
            if(update.isValid() && update > b.checkDate)
            {
              for(BugReport &bug : m_Ctx.Config().CrashReport_ReportedBugs)
              {
                if(bug.reportId == b.reportId)
                {
                  bug.unreadUpdates = true;
                  break;
                }
              }
              m_Ctx.Config().Save();
              PopulateReportedBugs();
            }
          });
        });
      }
    }
  });
  bugupdate->selfDelete(true);
  bugupdate->start();

  ui->toolWindowManager->setToolWindowCreateCallback([this](const QString &objectName) -> QWidget * {
    return m_Ctx.CreateBuiltinWindow(objectName);
  });

  ui->action_Start_Replay_Loop->setEnabled(false);
  ui->action_Open_RGP_Profile->setEnabled(false);
  ui->action_Create_RGP_Profile->setEnabled(false);
  ui->action_Resolve_Symbols->setEnabled(false);
  ui->action_Resolve_Symbols->setText(tr("Resolve Symbols"));

  ui->action_Recompress_Capture->setEnabled(false);
  ui->action_EmbedExternalFiles->setEnabled(false);
  ui->action_RemoveExternalFiles->setEnabled(false);

#if defined(Q_OS_WIN32)
#define SELF_HOST_NAME "rdocself.dll"
#else
#define SELF_HOST_NAME "librdocself.so"
#endif

  if(RENDERDOC_CanSelfHostedCapture(SELF_HOST_NAME))
  {
    QAction *begin = new QAction(tr("Start Self-hosted Capture"), this);
    QAction *end = new QAction(tr("End Self-hosted Capture"), this);
    end->setEnabled(false);

    QObject::connect(begin, &QAction::triggered, [begin, end]() {
      begin->setEnabled(false);
      end->setEnabled(true);

      RENDERDOC_StartSelfHostCapture(SELF_HOST_NAME);
    });

    QObject::connect(end, &QAction::triggered, [begin, end]() {
      begin->setEnabled(true);
      end->setEnabled(false);

      RENDERDOC_EndSelfHostCapture(SELF_HOST_NAME);
    });

    ui->menu_Tools->addSeparator();
    ui->menu_Tools->addAction(begin);
    ui->menu_Tools->addAction(end);
  }

  m_Ctx.AddCaptureViewer(this);

  ui->action_Save_Capture_Inplace->setEnabled(false);
  ui->action_Save_Capture_As->setEnabled(false);
  ui->action_Export_Project->setEnabled(false);
  ui->action_Close_Capture->setEnabled(false);
  ui->menu_Export_As->setEnabled(false);

  {
    ICaptureFile *tmp = RENDERDOC_OpenCaptureFile();
    rdcarray<CaptureFileFormat> formats = tmp->GetCaptureFileFormats();

    for(const CaptureFileFormat &fmt : formats)
    {
      if(fmt.extension == "rdc")
        continue;

      if(fmt.openSupported)
      {
        QAction *action = new QAction(fmt.name, this);

        QObject::connect(action, &QAction::triggered, [this, fmt]() { importCapture(fmt); });

        if(!fmt.description.isEmpty())
          action->setToolTip(fmt.description);

        ui->menu_Import_From->addAction(action);
      }

      if(fmt.convertSupported)
      {
        QAction *action = new QAction(fmt.name, this);

        QObject::connect(action, &QAction::triggered, [this, fmt]() { exportCapture(fmt); });

        if(!fmt.description.isEmpty())
          action->setToolTip(fmt.description);

        ui->menu_Export_As->addAction(action);
      }
    }

    tmp->Shutdown();
  }

  QList<QAction *> actions = ui->menuBar->actions();

  // register all the UI-designer created shortcut keys
  for(int i = 0; i < actions.count(); i++)
  {
    QAction *a = actions[i];

    QKeySequence ks = a->shortcut();
    if(!ks.isEmpty())
    {
      m_GlobalShortcutCallbacks[ks] = [a](QWidget *) {
        if(a->isEnabled())
          a->trigger();
      };
    }

    // recurse into submenus by appending to the end of the list.
    if(a->menu())
      actions.append(a->menu()->actions());
  }

  // hide the dummy extension markers. They shouldn't be visible, they're just there so the code can
  // easily find where to insert new extension menu items.
  ui->extension_dummy_File->setVisible(false);
  ui->extension_dummy_Window->setVisible(false);
  ui->extension_dummy_Tools->setVisible(false);
  ui->extension_dummy_Help->setVisible(false);

  RegisterShortcut("ALT+R", this, [this](QWidget *) { contextChooser->click(); });
}

MainWindow::~MainWindow()
{
  // close the network manager thread
  m_NetManagerThread->thread()->quit();
  m_NetManagerThread->deleteLater();

  m_Ctx.Replay().DisconnectFromRemoteServer();

  // explicitly delete our children here, so that the MainWindow is still alive while they are
  // closing.

  setUpdatesEnabled(false);
  qDeleteAll(findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly));

  m_RemoteProbeSemaphore.acquire();
  m_RemoteProbe->wait();
  m_RemoteProbe->deleteLater();
  delete ui;
}

QString MainWindow::GetLayoutPath(int layout)
{
  QString filename = lit("DefaultLayout.config");

  if(layout > 0)
    filename = lit("Layout%1.config").arg(layout);

  return ConfigFilePath(filename);
}

void MainWindow::on_action_Exit_triggered()
{
  this->close();
}

void MainWindow::on_action_Open_Capture_triggered()
{
  if(!PromptCloseCapture())
    return;

  QString filename = RDDialog::getOpenFileName(
      this, tr("Select file to open"), m_Ctx.Config().LastCaptureFilePath,
      tr("Capture Files (*.rdc);;Image Files (*.dds *.hdr *.exr *.bmp *.jpg "
         "*.jpeg *.png *.tga *.gif *.psd);;All Files (*)"));

  if(!filename.isEmpty())
    LoadFromFilename(filename, false);
}

void MainWindow::on_action_Open_Capture_with_Options_triggered()
{
  if(!PromptCloseCapture())
    return;

  ReplayOptionsSelector *replayOptions = new ReplayOptionsSelector(m_Ctx, true, this);

  QDialog *openWithOptions = new QDialog(this);
  openWithOptions->setWindowFlags(openWithOptions->windowFlags() & ~Qt::WindowContextHelpButtonHint);
  openWithOptions->setWindowIcon(windowIcon());
  openWithOptions->setWindowTitle(tr("Open Capture with Options"));
  openWithOptions->setSizeGripEnabled(false);
  openWithOptions->setModal(true);

  QVBoxLayout l;
  l.addWidget(replayOptions);
  l.setMargin(3);
  l.setSizeConstraint(QLayout::SetFixedSize);

  openWithOptions->setLayout(&l);

  QObject::connect(replayOptions, &ReplayOptionsSelector::canceled, openWithOptions,
                   &QDialog::reject);
  QObject::connect(replayOptions, &ReplayOptionsSelector::opened, openWithOptions, &QDialog::accept);

  if(RDDialog::show(openWithOptions) != QDialog::Accepted)
  {
    openWithOptions->deleteLater();
    return;
  }

  QString filename = replayOptions->filename();

  openWithOptions->deleteLater();

  if(filename.isEmpty())
    return;

  LoadCapture(filename, replayOptions->options(), false, true);
}

void MainWindow::importCapture(const CaptureFileFormat &fmt)
{
  if(!PromptCloseCapture())
    return;

  QString ext = fmt.extension;
  QString title = fmt.name;

  QString filename =
      RDDialog::getOpenFileName(this, tr("Select file to open"), QString(),
                                tr("%1 Files (*.%2);;All Files (*)").arg(title).arg(ext));

  if(!filename.isEmpty())
  {
    QString rdcfile = m_Ctx.TempCaptureFilename(lit("imported_") + ext);

    bool success = m_Ctx.ImportCapture(fmt, filename, rdcfile);

    if(success)
    {
      // open file as temporary, in case the user wants to save the imported rdc
      LoadFromFilename(rdcfile, true);
      takeCaptureOwnership();
    }
  }
}

void MainWindow::captureModified()
{
  // once the capture is modified, enable the save-in-place option. It might already have been
  // enabled if this capture was a temporary one
  if(m_Ctx.IsCaptureLoaded())
    ui->action_Save_Capture_Inplace->setEnabled(true);

  updateToolsMenuOptions();
}

void MainWindow::LoadFromFilename(const QString &filename, bool temporary)
{
  QFileInfo path(filename);
  QString ext = path.suffix().toLower();

  if(ext == lit("rdc"))
  {
    LoadCapture(filename, m_Ctx.Config().DefaultReplayOptions, temporary, true);
  }
  else if(ext == lit("cap"))
  {
    OpenCaptureConfigFile(filename, false);
  }
  else if(ext == lit("exe"))
  {
    OpenCaptureConfigFile(filename, true);
  }
  else
  {
    // not a recognised filetype, see if we can load it anyway
    LoadCapture(filename, m_Ctx.Config().DefaultReplayOptions, temporary, true);
  }
}

void MainWindow::OnCaptureTrigger(const QString &exe, const QString &workingDir,
                                  const QString &cmdLine,
                                  const rdcarray<EnvironmentModification> &env, CaptureOptions opts,
                                  std::function<void(LiveCapture *)> callback)
{
  if(!PromptCloseCapture())
    return;

  LambdaThread *th = new LambdaThread([this, exe, workingDir, cmdLine, env, opts, callback]() {
    if(isUnshareableDeviceInUse())
    {
      RDDialog::warning(this, tr("RenderDoc is already capturing an app on this device"),
                        tr("A running app on this device is already being captured with RenderDoc. "
                           "First please close the app then try to launch again."),
                        QMessageBox::Ok);
      return;
    }

    QString capturefile = m_Ctx.TempCaptureFilename(QFileInfo(exe).baseName());

    ExecuteResult ret =
        m_Ctx.Replay().ExecuteAndInject(exe, workingDir, cmdLine, env, capturefile, opts);

    GUIInvoke::call(this, [this, exe, ret, callback]() {
      if(ret.result.code == ResultCode::JDWPFailure)
      {
        RDDialog::critical(
            this, tr("Error connecting to debugger"),
            tr("<html>Error launching %1 for capture.\n\n"
               "Something went wrong connecting to the debugger on the Android device.\n\n"
               "This can happen if the package is not marked as debuggable, the device is not "
               "configured to allow app debugging, if the intent arguments are badly specified, or "
               "if another android tool such as Android Studio is interfering with the debug "
               "connection.\n\n"
               "Close <b>all</b> instances of Android Studio or other Android programs "
               "and try again.</html>")
                .arg(exe));
        return;
      }

      if(ret.result.code != ResultCode::Succeeded)
      {
        RDDialog::critical(
            this, tr("Error launching capture"),
            tr("Error launching %1 for capture.\n\n%2").arg(exe).arg(ret.result.Message()));
        return;
      }

      LiveCapture *live = new LiveCapture(
          m_Ctx,
          m_Ctx.Replay().CurrentRemote().IsValid() ? m_Ctx.Replay().CurrentRemote().Hostname() : "",
          m_Ctx.Replay().CurrentRemote().IsValid() ? m_Ctx.Replay().CurrentRemote().Name() : "",
          ret.ident, this, this);
      ShowLiveCapture(live);
      callback(live);
    });
  });
  th->setName(lit("ExecuteAndInject"));
  th->start();
  // wait a few ms before popping up a progress bar
  th->wait(500);
  if(th->isRunning())
  {
    QString filename = QFileInfo(exe).fileName();
    ShowProgressDialog(this, tr("Launching %1, please wait...").arg(filename),
                       [th]() { return !th->isRunning(); });
  }
  th->deleteLater();
}

void MainWindow::OnInjectTrigger(uint32_t PID, const rdcarray<EnvironmentModification> &env,
                                 const QString &name, CaptureOptions opts,
                                 std::function<void(LiveCapture *)> callback)
{
  if(!PromptCloseCapture())
    return;

  LambdaThread *th = new LambdaThread([this, PID, env, name, opts, callback]() {
    QString capturefile = m_Ctx.TempCaptureFilename(name);

    ExecuteResult ret = RENDERDOC_InjectIntoProcess(PID, env, capturefile, opts, false);

    GUIInvoke::call(this, [this, PID, ret, callback]() {
      if(ret.result.code != ResultCode::Succeeded)
      {
        RDDialog::critical(
            this, tr("Error injecting into process"),
            tr("Error injecting into process %1 for capture.\n\n%2").arg(PID).arg(ret.result.Message()));
        return;
      }

      LiveCapture *live = new LiveCapture(m_Ctx, QString(), QString(), ret.ident, this, this);
      ShowLiveCapture(live);
      callback(live);
    });
  });
  th->start();
  // wait a few ms before popping up a progress bar
  th->wait(500);
  if(th->isRunning())
  {
    ShowProgressDialog(this, tr("Injecting into %1, please wait...").arg(PID),
                       [th]() { return !th->isRunning(); });
  }
  th->deleteLater();
}

void MainWindow::LoadCapture(const QString &filename, const ReplayOptions &opts, bool temporary,
                             bool local)
{
  if(PromptCloseCapture())
  {
    if(m_Ctx.IsCaptureLoading())
      return;

    QString driver;
    QString machineIdent;
    ReplaySupport support = ReplaySupport::Unsupported;

    bool remoteReplay = !local || m_Ctx.Replay().CurrentRemote().IsConnected();

    if(local)
    {
      ICaptureFile *file = RENDERDOC_OpenCaptureFile();

      ResultDetails result = file->OpenFile(filename, "rdc", NULL);

      if(!result.OK())
      {
        RDDialog::critical(this, tr("Error opening capture"),
                           tr("Couldn't open file '%1'\n%2").arg(filename).arg(result.Message()));

        file->Shutdown();
        return;
      }

      driver = file->DriverName();
      machineIdent = file->RecordedMachineIdent();
      support = file->LocalReplaySupport();

      file->Shutdown();

      // if the return value suggests remote replay, and it's not already selected, AND the user
      // hasn't previously chosen to always replay locally without being prompted, ask if they'd
      // prefer to switch to a remote context for replaying.
      if(support == ReplaySupport::SuggestRemote && !remoteReplay &&
         !m_Ctx.Config().AlwaysReplayLocally)
      {
        SuggestRemoteDialog dialog(driver, machineIdent, this);

        FillRemotesMenu(dialog.remotesMenu(), false);

        dialog.remotesAdded();

        RDDialog::show(&dialog);

        if(dialog.choice() == SuggestRemoteDialog::Cancel)
        {
          return;
        }
        else if(dialog.choice() == SuggestRemoteDialog::Remote)
        {
          // we only get back here from the dialog once the context switch has begun,
          // so contextChooser will have been disabled.
          // Check once to see if it's enabled before even popping up the dialog in case
          // it has finished already. Otherwise pop up a waiting dialog until it completes
          // one way or another, then process the result.

          if(!contextChooser->isEnabled())
          {
            ShowProgressDialog(this, tr("Please Wait - Checking remote connection..."),
                               [this]() { return contextChooser->isEnabled(); });
          }

          remoteReplay = m_Ctx.Replay().CurrentRemote().IsConnected();

          if(!remoteReplay)
          {
            QString remoteMessage = tr("Failed to make a connection to the remote server.\n\n");

            remoteMessage += tr("More information may be available in the status bar.");

            RDDialog::information(this, tr("Couldn't connect to remote server"), remoteMessage);
            return;
          }
        }
        else
        {
          // nothing to do - we just continue replaying locally
          // however we need to check if the user selected 'always replay locally' and
          // set that bit as sticky in the config
          if(dialog.alwaysReplayLocally())
          {
            m_Ctx.Config().AlwaysReplayLocally = true;

            m_Ctx.Config().Save();
          }
        }
      }

      if(remoteReplay)
      {
        support = ReplaySupport::Unsupported;

        rdcarray<rdcstr> remoteDrivers = m_Ctx.Replay().GetRemoteSupport();

        for(const rdcstr &d : remoteDrivers)
        {
          if(driver == QString(d))
            support = ReplaySupport::Supported;
        }
      }
    }

    QString origFilename = filename;

    // if driver is empty something went wrong loading the capture, let it be handled as usual
    // below. Otherwise indicate that support is missing.
    if(!driver.isEmpty() && support == ReplaySupport::Unsupported)
    {
      if(remoteReplay)
      {
        QString remoteMessage =
            tr("This capture was captured with %1 and cannot be replayed on %2.\n\n")
                .arg(driver)
                .arg(m_Ctx.Replay().CurrentRemote().Name());

        remoteMessage += tr("Try selecting a different remote context in the status bar.");

        RDDialog::critical(this, tr("Unsupported capture driver"), remoteMessage);
      }
      else
      {
        QString remoteMessage =
            tr("This capture was captured with %1 and cannot be replayed locally.\n\n").arg(driver);

        remoteMessage += tr("Try selecting a remote context in the status bar.");

        RDDialog::critical(this, tr("Unsupported capture driver"), remoteMessage);
      }

      return;
    }
    else
    {
      QString fileToLoad = filename;

      if(remoteReplay && local)
      {
        fileToLoad = m_Ctx.Replay().CopyCaptureToRemote(filename, this);

        // deliberately leave local as true so that we keep referring to the locally saved capture

        // some error
        if(fileToLoad.isEmpty())
        {
          RDDialog::critical(this, tr("Error copying to remote"),
                             tr("Couldn't copy %1 to remote host for replaying").arg(filename));
          return;
        }
      }

      statusText->setText(tr("Loading %1...").arg(origFilename));

      if(driver == lit("Image"))
      {
        ANALYTIC_SET(UIFeatures.ImageViewer, true);
      }

      m_Ctx.LoadCapture(fileToLoad, opts, origFilename, temporary, local);
    }

    if(local && !temporary)
    {
      m_Ctx.Config().LastCaptureFilePath = QFileInfo(filename).absolutePath();
    }
  }
}

void MainWindow::OpenCaptureConfigFile(const QString &filename, bool exe)
{
  ICaptureDialog *capDialog = m_Ctx.GetCaptureDialog();

  if(exe)
    capDialog->SetExecutableFilename(filename);
  else
    capDialog->LoadSettings(filename);

  if(!ui->toolWindowManager->toolWindows().contains(capDialog->Widget()))
    ui->toolWindowManager->addToolWindow(capDialog->Widget(), mainToolArea());

  ToolWindowManager::raiseToolWindow(capDialog->Widget());
}

QString MainWindow::GetSavePath(QString title, QString filter)
{
  QString dir;

  if(!m_Ctx.Config().DefaultCaptureSaveDirectory.isEmpty())
  {
    if(m_LastSaveCapturePath.isEmpty())
      dir = m_Ctx.Config().DefaultCaptureSaveDirectory;
    else
      dir = m_LastSaveCapturePath;
  }

  if(title.isEmpty())
    title = tr("Save Capture As");

  if(filter.isEmpty())
    filter = tr("Capture Files (*.rdc)");

  QString filename = RDDialog::getSaveFileName(this, title, dir, filter);

  if(!filename.isEmpty())
  {
    QDir dirinfo = QFileInfo(filename).dir();
    if(dirinfo.exists())
      m_LastSaveCapturePath = dirinfo.absolutePath();

    return filename;
  }

  return QString();
}

bool MainWindow::PromptSaveCaptureAs()
{
  QString saveFilename = GetSavePath();

  if(!saveFilename.isEmpty())
    return SaveCurrentCapture(saveFilename);

  return false;
}

bool MainWindow::SaveCurrentCapture(QString saveFilename)
{
  QString origFilename = m_Ctx.GetCaptureFilename();

  bool success = m_Ctx.SaveCaptureTo(saveFilename);

  if(!success)
    return false;

  AddRecentFile(m_Ctx.Config().RecentCaptureFiles, saveFilename);
  PopulateRecentCaptureFiles();
  SetTitle(saveFilename);

  for(LiveCapture *live : m_LiveCaptures)
    live->fileSaved(origFilename, saveFilename);

  ui->action_Save_Capture_Inplace->setEnabled(false);

  return true;
}

void MainWindow::exportCapture(const CaptureFileFormat &fmt)
{
  if(!m_Ctx.IsCaptureLocal())
  {
    RDDialog::information(
        this, tr("Save changes to capture?"),
        tr("The capture is on a remote host, it must be saved locally before it can be exported."));
    PromptSaveCaptureAs();
    return;
  }

  QString saveFilename =
      GetSavePath(tr("Export Capture As"),
                  tr("%1 Files (*.%2)").arg(QString(fmt.name)).arg(QString(fmt.extension)));

  if(!saveFilename.isEmpty())
    m_Ctx.ExportCapture(fmt, saveFilename);
}

bool MainWindow::PromptCloseCapture()
{
  if(!m_Ctx.IsCaptureLoaded())
    return true;

  QString deletepath;
  bool caplocal = false;

  if(m_OwnTempCapture && m_Ctx.IsCaptureTemporary())
  {
    QString temppath = m_Ctx.GetCaptureFilename();
    caplocal = m_Ctx.IsCaptureLocal();

    QMessageBox::StandardButton res =
        RDDialog::question(this, tr("Unsaved capture"), tr("Save this capture?"),
                           QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

    if(res == QMessageBox::Cancel)
      return false;

    if(res == QMessageBox::Yes)
    {
      bool success = PromptSaveCaptureAs();

      if(!success)
      {
        return false;
      }
    }

    if(temppath != m_Ctx.GetCaptureFilename() || res == QMessageBox::No)
      deletepath = temppath;
    m_OwnTempCapture = false;
  }
  else if(m_Ctx.GetCaptureModifications() != CaptureModifications::NoModifications)
  {
    QString text = tr("This capture has the following modifications:\n\n");

    CaptureModifications mods = m_Ctx.GetCaptureModifications();

    if(mods & CaptureModifications::Renames)
      text += tr("Resources have been renamed.\n");
    if(mods & CaptureModifications::Bookmarks)
      text += tr("Bookmarks have been changed.\n");
    if(mods & CaptureModifications::Notes)
      text += tr("Capture notes have been changed.\n");
    if(mods & CaptureModifications::EditedShaders)
      text += tr("Edited shaders have been changed.\n");

    bool saveas = false;

    if(m_Ctx.IsCaptureLocal())
    {
      text += tr("\nWould you like to save those changes to '%1'?").arg(m_Ctx.GetCaptureFilename());
    }
    else
    {
      saveas = true;
      text +=
          tr("\nThe capture is on a remote host, would you like to save these changes locally?");
    }

    QMessageBox::StandardButton res =
        RDDialog::question(this, tr("Save changes to capture?"), text,
                           QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

    if(res == QMessageBox::Cancel)
      return false;

    if(res == QMessageBox::Yes)
    {
      bool success = false;

      if(saveas)
        success = PromptSaveCaptureAs();
      else
        success = m_Ctx.SaveCaptureTo(m_Ctx.GetCaptureFilename());

      if(!success)
        return false;
    }
  }

  CloseCapture();

  if(!deletepath.isEmpty())
  {
    m_Ctx.Replay().DeleteCapture(deletepath, caplocal);
    RemoveRecentCapture(deletepath);
  }

  return true;
}

void MainWindow::CloseCapture()
{
  QString path = m_Ctx.GetCaptureFilename();
  bool local = m_Ctx.IsCaptureLocal();
  bool temp = m_Ctx.IsCaptureTemporary();

  m_Ctx.CloseCapture();

  if(m_OwnTempCapture && temp)
  {
    m_Ctx.Replay().DeleteCapture(path, local);
    RemoveRecentCapture(path);
    m_OwnTempCapture = false;
  }

  ui->action_Save_Capture_Inplace->setEnabled(false);
  ui->action_Save_Capture_As->setEnabled(false);
  ui->action_Export_Project->setEnabled(false);
  ui->menu_Export_As->setEnabled(false);
}

void MainWindow::SetTitle(const QString &filename)
{
  QString prefix;

  if(m_Ctx.IsCaptureLoaded())
  {
    prefix = QFileInfo(filename).fileName();
    if(m_Ctx.APIProps().degraded)
      prefix += tr(" !DEGRADED PERFORMANCE!");
    prefix += lit(" - ");
  }

  if(m_Ctx.Replay().CurrentRemote().IsValid())
    prefix += tr("Remote: %1 - ").arg(m_Ctx.Replay().CurrentRemote().Name());

  QString text = prefix + lit("RenderDoc ");

  if(RENDERDOC_STABLE_BUILD)
    text += lit(FULL_VERSION_STRING);
  else
    text += tr("Unstable %1 Build (%2 - %3)")
                .arg(RENDERDOC_IsReleaseBuild() ? lit("Release") : lit("Development"))
                .arg(lit(FULL_VERSION_STRING))
                .arg(QString::fromLatin1(RENDERDOC_GetCommitHash()));

  if(IsRunningAsAdmin())
    text += tr(" (Administrator)");

  if(QString::fromLatin1(RENDERDOC_GetVersionString()) != lit(MAJOR_MINOR_VERSION_STRING))
    text += tr(" - !! VERSION MISMATCH DETECTED !!");

  setWindowTitle(text);
}

void MainWindow::SetTitle()
{
  SetTitle(m_Ctx.GetCaptureFilename());
}

bool MainWindow::HandleMismatchedVersions()
{
  if(IsVersionMismatched())
  {
    qCritical() << "Version mismatch between UI (" << lit(MAJOR_MINOR_VERSION_STRING) << ")"
                << "and core"
                << "(" << QString::fromUtf8(RENDERDOC_GetVersionString()) << ")";

#if !RENDERDOC_OFFICIAL_BUILD
    RDDialog::critical(
        this, tr("Unofficial build - mismatched versions"),
        tr("You are running an unofficial build with mismatched core and UI versions.\n"
           "Double check where you got your build from and do a sanity check!"));
#else
    QMessageBox::StandardButton res = RDDialog::critical(
        this, tr("Mismatched versions"),
        tr("RenderDoc has detected mismatched versions between its internal module and UI.\n"
           "This is likely caused by a buggy update in the past which partially updated your "
           "install."
           "Likely because a program was running with renderdoc while the update happened.\n"
           "You should reinstall RenderDoc immediately as this configuration is almost guaranteed "
           "to crash.\n\n"
           "Would you like to open the downloads page to reinstall?"),
        QMessageBox::Yes | QMessageBox::No);

    if(res == QMessageBox::Yes)
      QDesktopServices::openUrl(QUrl(lit("https://renderdoc.org/builds")));

    SetUpdateAvailable();
#endif
    return true;
  }

  return false;
}

bool MainWindow::IsVersionMismatched()
{
  return QString::fromLatin1(RENDERDOC_GetVersionString()) != lit(MAJOR_MINOR_VERSION_STRING);
}

void MainWindow::ClearRecentCaptureFiles()
{
  m_Ctx.Config().RecentCaptureFiles.clear();
  PopulateRecentCaptureFiles();
}

void MainWindow::PopulateRecentCaptureFiles()
{
  ui->menu_Recent_Capture_Files->clear();

  ui->menu_Recent_Capture_Files->setEnabled(false);

  int idx = 1;
  for(int i = m_Ctx.Config().RecentCaptureFiles.count() - 1; i >= 0; i--)
  {
    QString filename = m_Ctx.Config().RecentCaptureFiles[i];
    QString filenameDisplay = filename;
    filenameDisplay.replace(QLatin1Char('&'), lit("&&"));
    ui->menu_Recent_Capture_Files->addAction(QFormatStr("&%1 %2").arg(idx).arg(filenameDisplay),
                                             [this, filename] { recentCaptureFile(filename); });
    idx++;

    ui->menu_Recent_Capture_Files->setEnabled(true);

    // only populate the 9 most recent, even if more exist in memory
    if(idx == 10)
      break;
  }

  ui->menu_Recent_Capture_Files->addSeparator();
  ui->menu_Recent_Capture_Files->addAction(ui->action_Clear_Capture_Files_History);
}

void MainWindow::ClearRecentCaptureSettings()
{
  m_Ctx.Config().RecentCaptureSettings.clear();
  PopulateRecentCaptureSettings();
}

void MainWindow::networkRequestFailed(QUrl url, QString error)
{
  if(m_NetworkFailCallbacks.contains(url))
  {
    m_NetworkFailCallbacks[url](error);
    m_NetworkFailCallbacks.remove(url);
  }
}

void MainWindow::networkRequestCompleted(QUrl url, QByteArray replyData)
{
  if(m_NetworkCompleteCallbacks.contains(url))
  {
    m_NetworkCompleteCallbacks[url](replyData);
    m_NetworkCompleteCallbacks.remove(url);
  }
}

void MainWindow::PopulateRecentCaptureSettings()
{
  ui->menu_Recent_Capture_Settings->clear();

  ui->menu_Recent_Capture_Settings->setEnabled(false);

  int idx = 1;
  for(int i = m_Ctx.Config().RecentCaptureSettings.count() - 1; i >= 0; i--)
  {
    QString filename = m_Ctx.Config().RecentCaptureSettings[i];
    QString filenameDisplay = filename;
    filenameDisplay.replace(QLatin1Char('&'), lit("&&"));
    ui->menu_Recent_Capture_Settings->addAction(QFormatStr("&%1 %2").arg(idx).arg(filenameDisplay),
                                                [this, filename] { recentCaptureSetting(filename); });
    idx++;

    ui->menu_Recent_Capture_Settings->setEnabled(true);

    // only populate the 9 most recent, even if more exist in memory
    if(idx == 10)
      break;
  }

  ui->menu_Recent_Capture_Settings->addSeparator();
  ui->menu_Recent_Capture_Settings->addAction(ui->action_Clear_Capture_Settings_History);
}

void MainWindow::on_action_Clear_Reported_Bugs_triggered()
{
  ui->menu_Reported_Bugs->clear();
  ui->menu_Reported_Bugs->setEnabled(false);

  m_Ctx.Config().CrashReport_ReportedBugs.clear();
  m_Ctx.Config().Save();
}

void MainWindow::PopulateReportedBugs()
{
  ui->menu_Reported_Bugs->clear();

  ui->menu_Reported_Bugs->setEnabled(false);

  bool unread = false;

  int idx = 1;
  for(int i = m_Ctx.Config().CrashReport_ReportedBugs.count() - 1; i >= 0; i--)
  {
    BugReport &bug = m_Ctx.Config().CrashReport_ReportedBugs[i];
    QString fmt = tr("&%1: Bug reported at %2");

    if(bug.unreadUpdates)
      fmt = tr("&%1: (Update) Bug reported at %2");

    QAction *action = ui->menu_Reported_Bugs->addAction(
        fmt.arg(idx).arg(QDateTime(bug.submitDate).toString()), [this, i] {
          BugReport &bug = m_Ctx.Config().CrashReport_ReportedBugs[i];

          QDesktopServices::openUrl(QString(bug.URL()));

          bug.unreadUpdates = false;
          m_Ctx.Config().Save();

          PopulateReportedBugs();
        });
    idx++;

    if(bug.unreadUpdates)
    {
      action->setIcon(Icons::bug());
      unread = true;
    }

    ui->menu_Reported_Bugs->setEnabled(true);
  }

  ui->menu_Reported_Bugs->addSeparator();
  ui->menu_Reported_Bugs->addAction(ui->action_Clear_Reported_Bugs);

  if(unread)
  {
    ui->menu_Help->setIcon(Icons::bug());
    ui->menu_Reported_Bugs->setIcon(Icons::bug());
  }
  else
  {
    ui->menu_Help->setIcon(QIcon());
    ui->menu_Reported_Bugs->setIcon(QIcon());
  }
}

void MainWindow::CheckUpdates(bool forceCheck, UpdateResultMethod callback)
{
  if(!updateAction)
    return;

  bool mismatch = HandleMismatchedVersions();
  if(mismatch)
    return;

  if(!forceCheck && !m_Ctx.Config().CheckUpdate_AllowChecks)
  {
    updateAction->setVisible(false);
    if(callback)
      callback(UpdateResult::Disabled);
    return;
  }

#if RENDERDOC_OFFICIAL_BUILD

  // if the current version isn't the one we expected, clear any cached update state
  if(m_Ctx.Config().CheckUpdate_CurrentVersion != MAJOR_MINOR_VERSION_STRING)
  {
    m_Ctx.Config().CheckUpdate_UpdateAvailable = false;
    m_Ctx.Config().CheckUpdate_UpdateResponse = "";
    m_Ctx.Config().CheckUpdate_CurrentVersion = MAJOR_MINOR_VERSION_STRING;
  }

  QDateTime today = QDateTime::currentDateTime();

  // check by default every 2 days
  QDateTime compare = today.addDays(-2);

  // if there's already an update available, go down to checking every week.
  if(m_Ctx.Config().CheckUpdate_UpdateAvailable)
    compare = today.addDays(-7);

  bool checkDue = compare.secsTo(m_Ctx.Config().CheckUpdate_LastUpdate) < 0;

  if(m_Ctx.Config().CheckUpdate_UpdateAvailable)
  {
    // Mark an update available
    SetUpdateAvailable();

    // If we don't have a proper update response, or we're overdue for a check, then do it again.
    // The reason for this is twofold: first, if someone has been delaying their updates for a long
    // time then there might be a newer update available that we should refresh to, so we should
    // find out and refresh the update status. The other reason is that when we get a positive
    // response from the server we force-display the popup which means the user will get reminded
    // every week or so that an update is pending.
    if(m_Ctx.Config().CheckUpdate_UpdateResponse.isEmpty() || checkDue)
    {
      forceCheck = true;
    }

    // If we're not forcing a recheck, we're done.
    if(!forceCheck)
      return;
  }

  if(!forceCheck && !checkDue)
  {
    if(callback)
      callback(UpdateResult::Toosoon);
    return;
  }

  m_Ctx.Config().CheckUpdate_LastUpdate = today;
  m_Ctx.Config().Save();

#if QT_POINTER_SIZE == 4
  QString bitness = lit("32");
#else
  QString bitness = lit("64");
#endif
  QString versionCheck = lit(MAJOR_MINOR_VERSION_STRING);

  statusText->setText(tr("Checking for updates..."));

  statusProgress->setVisible(true);
  statusProgress->setMaximum(0);

  // call out to the status-check to see when the bug report was last updated
  MakeNetworkRequest(
      QUrl(lit("https://renderdoc.org/getupdateurl/%1/%2?htmlnotes=1").arg(bitness).arg(versionCheck)),

      // on success
      [this, callback](QByteArray replyData) {
        statusText->setText(QString());
        statusProgress->setVisible(false);

        QString response = QString::fromUtf8(replyData);

        if(response.isEmpty())
        {
          m_Ctx.Config().CheckUpdate_UpdateAvailable = false;
          m_Ctx.Config().CheckUpdate_UpdateResponse = "";
          m_Ctx.Config().CheckUpdate_CurrentVersion = lit(MAJOR_MINOR_VERSION_STRING);
          m_Ctx.Config().Save();
          SetNoUpdate();

          if(callback)
            callback(UpdateResult::Latest);

          return;
        }

        m_Ctx.Config().CheckUpdate_UpdateAvailable = true;
        m_Ctx.Config().CheckUpdate_UpdateResponse = response;
        m_Ctx.Config().CheckUpdate_CurrentVersion = lit(MAJOR_MINOR_VERSION_STRING);
        m_Ctx.Config().Save();
        SetUpdateAvailable();
        UpdatePopup();
      },

      // on error
      [this](QString error) {
        statusText->setText(QString());
        statusProgress->setVisible(false);
        qCritical() << "Network error checking for updates:" << error;
      });
#else    //! RENDERDOC_OFFICIAL_BUILD
  {
    if(callback)
      callback(UpdateResult::Unofficial);
    return;
  }
#endif
}

void MainWindow::SetUpdateAvailable()
{
  if(updateAction)
    updateAction->setVisible(true);
}

void MainWindow::SetNoUpdate()
{
  if(updateAction)
    updateAction->setVisible(false);
}

void MainWindow::UpdatePopup()
{
  if(!m_Ctx.Config().CheckUpdate_UpdateAvailable || !m_Ctx.Config().CheckUpdate_AllowChecks)
    return;

  UpdateDialog update((QString)m_Ctx.Config().CheckUpdate_UpdateResponse);
  RDDialog::show(&update);
}

void MainWindow::ShowLiveCapture(LiveCapture *live)
{
  m_LiveCaptures.push_back(live);

  if(m_Ctx.HasCaptureDialog())
    m_Ctx.AddDockWindow(live, DockReference::AddTo, m_Ctx.GetCaptureDialog()->Widget());
  else
    m_Ctx.AddDockWindow(live, DockReference::MainToolArea, this);
}

void MainWindow::LiveCaptureClosed(LiveCapture *live)
{
  m_LiveCaptures.removeOne(live);
}

QMenu *MainWindow::GetBaseMenu(WindowMenu base, rdcstr name)
{
  switch(base)
  {
    case WindowMenu::File: return ui->menu_File;
    case WindowMenu::Window: return ui->menu_Window;
    case WindowMenu::Tools: return ui->menu_Tools;
    case WindowMenu::Help: return ui->menu_Help;
    case WindowMenu::NewMenu: break;
    default: return NULL;
  }

  // new menu. See if we have one for name already. If not, create a new one and add it to the menu
  // bar.
  for(QAction *m : ui->menuBar->actions())
  {
    // if it has an object name it's a built-in, ignore it.
    if(!m->objectName().isEmpty())
      continue;

    if(m->text() == name)
      return m->menu();
  }

  // no existing menu, add a new one
  QMenu *menu = new QMenu(name, this);
  menu->setIcon(Icons::plugin());
  ui->menuBar->insertMenu(ui->menu_Help->menuAction(), menu);
  return menu;
}

QList<QAction *> MainWindow::GetMenuActions()
{
  return ui->menuBar->actions();
}

ToolWindowManager *MainWindow::mainToolManager()
{
  return ui->toolWindowManager;
}

ToolWindowManager::AreaReference MainWindow::mainToolArea()
{
  // bit of a hack. Maybe the ToolWindowManager should track this?
  // Try and identify where to add new windows, by searching a
  // priority list of other windows to use their area
  if(m_Ctx.HasTextureViewer() &&
     ui->toolWindowManager->toolWindows().contains(m_Ctx.GetTextureViewer()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::AddTo, ui->toolWindowManager->areaOf(m_Ctx.GetTextureViewer()->Widget()));
  else if(m_Ctx.HasPipelineViewer() &&
          ui->toolWindowManager->toolWindows().contains(m_Ctx.GetPipelineViewer()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::AddTo, ui->toolWindowManager->areaOf(m_Ctx.GetPipelineViewer()->Widget()));
  else if(m_Ctx.HasMeshPreview() &&
          ui->toolWindowManager->toolWindows().contains(m_Ctx.GetMeshPreview()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::AddTo, ui->toolWindowManager->areaOf(m_Ctx.GetMeshPreview()->Widget()));
  else if(m_Ctx.HasCaptureDialog() &&
          ui->toolWindowManager->toolWindows().contains(m_Ctx.GetCaptureDialog()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::AddTo, ui->toolWindowManager->areaOf(m_Ctx.GetCaptureDialog()->Widget()));

  // if all else fails just add to the last place we placed something.
  return ToolWindowManager::AreaReference(ToolWindowManager::LastUsedArea);
}

ToolWindowManager::AreaReference MainWindow::leftToolArea()
{
  // see mainToolArea()
  if(m_Ctx.HasTextureViewer() &&
     ui->toolWindowManager->toolWindows().contains(m_Ctx.GetTextureViewer()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::LeftOf, ui->toolWindowManager->areaOf(m_Ctx.GetTextureViewer()->Widget()));
  else if(m_Ctx.HasPipelineViewer() &&
          ui->toolWindowManager->toolWindows().contains(m_Ctx.GetPipelineViewer()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::LeftOf,
        ui->toolWindowManager->areaOf(m_Ctx.GetPipelineViewer()->Widget()));
  else if(m_Ctx.HasCaptureDialog() &&
          ui->toolWindowManager->toolWindows().contains(m_Ctx.GetCaptureDialog()->Widget()))
    return ToolWindowManager::AreaReference(
        ToolWindowManager::LeftOf, ui->toolWindowManager->areaOf(m_Ctx.GetCaptureDialog()->Widget()));

  return ToolWindowManager::AreaReference(ToolWindowManager::LastUsedArea);
}

void MainWindow::BringToFront()
{
  // un-minimise if necessary
  setWindowState(windowState() & ~Qt::WindowMinimized);
  show();
  raise();
  activateWindow();
}

void MainWindow::LoadInitialLayout()
{
  bool loaded = LoadLayout(0);

  // create default layout if layout failed to load
  if(!loaded)
  {
    QWidget *eventBrowser = m_Ctx.GetEventBrowser()->Widget();

    ui->toolWindowManager->addToolWindow(eventBrowser, ToolWindowManager::EmptySpace);

    QWidget *textureViewer = m_Ctx.GetTextureViewer()->Widget();

    ui->toolWindowManager->addToolWindow(
        textureViewer,
        ToolWindowManager::AreaReference(ToolWindowManager::RightOf,
                                         ui->toolWindowManager->areaOf(eventBrowser), 0.75f));

    QWidget *pipe = m_Ctx.GetPipelineViewer()->Widget();

    ui->toolWindowManager->addToolWindow(
        pipe, ToolWindowManager::AreaReference(ToolWindowManager::AddTo,
                                               ui->toolWindowManager->areaOf(textureViewer)));

    QWidget *mesh = m_Ctx.GetMeshPreview()->Widget();

    ui->toolWindowManager->addToolWindow(
        mesh, ToolWindowManager::AreaReference(ToolWindowManager::AddTo,
                                               ui->toolWindowManager->areaOf(textureViewer)));

    QWidget *capDialog = m_Ctx.GetCaptureDialog()->Widget();

    ui->toolWindowManager->addToolWindow(
        capDialog, ToolWindowManager::AreaReference(ToolWindowManager::AddTo,
                                                    ui->toolWindowManager->areaOf(textureViewer)));

    QWidget *apiInspector = m_Ctx.GetAPIInspector()->Widget();

    ui->toolWindowManager->addToolWindow(
        apiInspector,
        ToolWindowManager::AreaReference(ToolWindowManager::BottomOf,
                                         ui->toolWindowManager->areaOf(eventBrowser), 0.3f));

    QWidget *timelineBar = m_Ctx.GetTimelineBar()->Widget();

    ui->toolWindowManager->addToolWindow(
        timelineBar,
        ToolWindowManager::AreaReference(ToolWindowManager::TopWindowSide,
                                         ui->toolWindowManager->areaOf(textureViewer), 0.2f));
  }
}

bool MainWindow::ErrorReportsAllowed()
{
  return ui->action_Send_Error_Report->isEnabled();
}

void MainWindow::RemoveRecentCapture(const QString &filename)
{
  RemoveRecentFile(m_Ctx.Config().RecentCaptureFiles, filename);

  PopulateRecentCaptureFiles();
}

void MainWindow::recentCaptureFile(const QString &filename)
{
  if(QFileInfo::exists(filename))
  {
    LoadCapture(filename, m_Ctx.Config().DefaultReplayOptions, false, true);
  }
  else
  {
    QMessageBox::StandardButton res =
        RDDialog::question(this, tr("File not found"),
                           tr("File %1 couldn't be found.\nRemove from recent list?").arg(filename));

    if(res == QMessageBox::Yes)
    {
      RemoveRecentCapture(filename);
    }
  }
}

void MainWindow::recentCaptureSetting(const QString &filename)
{
  if(QFileInfo::exists(filename))
  {
    OpenCaptureConfigFile(filename, false);
  }
  else
  {
    QMessageBox::StandardButton res =
        RDDialog::question(this, tr("File not found"),
                           tr("File %1 couldn't be found.\nRemove from recent list?").arg(filename));

    if(res == QMessageBox::Yes)
    {
      m_Ctx.Config().RecentCaptureSettings.removeOne(filename);

      PopulateRecentCaptureSettings();
    }
  }
}

void MainWindow::setProgress(float val)
{
  if(val < 0.0f || val >= 1.0f)
  {
    statusProgress->setVisible(false);
    statusText->setText(QString());
  }
  else
  {
    statusProgress->setVisible(true);
    statusProgress->setMaximum(1000);
    statusProgress->setValue(1000 * val);
  }
}

void MainWindow::setCaptureHasErrors(bool errors)
{
  QString filename = QFileInfo(m_Ctx.GetCaptureFilename()).fileName();
  if(errors)
  {
    const QPixmap &del = Pixmaps::del(this);
    QPixmap empty(del.width(), del.height());
    empty.setDevicePixelRatio(del.devicePixelRatio());
    empty.fill(Qt::transparent);
    statusIcon->setPixmap(m_messageAlternate ? empty : del);

    QString text;
    text = tr("%1 loaded. Capture has %2 issues.").arg(filename).arg(m_Ctx.DebugMessages().size());
    if(m_Ctx.UnreadMessageCount() > 0)
      text += tr(" %1 Unread.").arg(m_Ctx.UnreadMessageCount());
    statusText->setText(text);
  }
  else
  {
    statusIcon->setPixmap(Pixmaps::tick(this));
    statusText->setText(tr("%1 loaded. No problems detected.").arg(filename));
  }
}

void MainWindow::remoteProbe()
{
  if(!m_Ctx.IsCaptureLoaded() && !m_Ctx.IsCaptureLoading())
  {
    m_Ctx.Config().UpdateEnumeratedProtocolDevices();

    // fetch the latest list
    rdcarray<RemoteHost> hosts = m_Ctx.Config().GetRemoteHosts();

    for(RemoteHost &host : hosts)
    {
      // don't mess with a host we're connected to - this is handled anyway
      if(host.IsConnected())
        continue;

      // this will do the bulk of the status checking on this thread without holding any lock, then
      // grab the remote host lock and update the config's host (if it's still there)
      host.CheckStatus();

      // bail as soon as we notice that we're done
      if(!m_RemoteProbeSemaphore.available())
        return;
    }
  }
}

void MainWindow::messageCheck()
{
  if(m_Ctx.IsCaptureLoaded())
  {
    if(m_Ctx.Replay().GetCurrentProcessingTime() >= 1.5f)
    {
      statusProgress->setVisible(true);
      statusProgress->setMaximum(0);
    }
    else
    {
      statusProgress->hide();
    }

    m_Ctx.Replay().AsyncInvoke([this](IReplayController *r) {
      rdcarray<DebugMessage> msgs;

      bool disconnected = false;

      if(m_Ctx.Replay().CurrentRemote().IsValid())
      {
        bool wasRunning = m_Ctx.Replay().CurrentRemote().IsServerRunning();

        m_Ctx.Replay().PingRemote();

        if(wasRunning != m_Ctx.Replay().CurrentRemote().IsServerRunning())
        {
          qCritical() << "Remote server disconnected";
          disconnected = true;
        }

        if(!disconnected && wasRunning)
          msgs = r->GetDebugMessages();
      }
      else
      {
        msgs = r->GetDebugMessages();
      }

      GUIInvoke::call(this, [this, msgs] {
        if(m_Ctx.Replay().CurrentRemote().IsValid() &&
           !m_Ctx.Replay().CurrentRemote().IsServerRunning())
          contextChooser->setIcon(Icons::cross());

        if(!msgs.empty())
        {
          m_Ctx.AddMessages(msgs);
        }

        if(m_Ctx.UnreadMessageCount() > 0)
          m_messageAlternate = !m_messageAlternate;
        else
          m_messageAlternate = false;

        setCaptureHasErrors(!m_Ctx.DebugMessages().empty());
      });
    });
  }
  else if(!m_Ctx.IsCaptureLoaded() && !m_Ctx.IsCaptureLoading())
  {
    if(m_Ctx.Replay().CurrentRemote().IsValid())
      m_Ctx.Replay().PingRemote();

    GUIInvoke::call(this, [this]() {
      if(m_Ctx.Replay().CurrentRemote().IsValid() && !m_Ctx.Replay().CurrentRemote().IsServerRunning())
      {
        contextChooser->setIcon(Icons::cross());
        contextChooser->setText(tr("Replay Context: %1").arg(tr("Local")));
        statusText->setText(
            tr("Remote server disconnected. To attempt to reconnect please select it again."));

        m_Ctx.Replay().DisconnectFromRemoteServer();
      }

      if(m_Ctx.HasCaptureDialog())
        m_Ctx.GetCaptureDialog()->UpdateRemoteHost();
    });
  }
}

void MainWindow::FillRemotesMenu(QMenu *menu, bool includeLocalhost)
{
  menu->clear();

  rdcarray<RemoteHost> hosts = m_Ctx.Config().GetRemoteHosts();

  int idx = 1;

  for(int i = 0; i < hosts.count(); i++)
  {
    RemoteHost host = hosts[i];

    // add localhost at the end, skip invalid hosts
    if(host.IsLocalhost() || !host.IsValid())
      continue;

    QAction *action = new QAction(menu);

    action->setIcon(host.IsServerRunning() && !host.IsVersionMismatch() ? Icons::tick()
                                                                        : Icons::cross());
    if(host.IsConnected())
      action->setText(tr("%1 (Connected)").arg(host.Name()));
    else if(host.IsServerRunning() && host.IsVersionMismatch())
      action->setText(tr("%1 (%2)").arg(host.Name(), host.VersionMismatchError()));
    else if(host.IsServerRunning() && host.IsBusy())
      action->setText(tr("%1 (Busy)").arg(host.Name()));
    else if(host.IsServerRunning())
      action->setText(tr("%1 (Online)").arg(host.Name()));
    else
      action->setText(tr("%1 (Offline)").arg(host.Name()));

    action->setText(lit("%1: %2").arg(idx++).arg(action->text()));

    QObject::connect(action, &QAction::triggered, this, &MainWindow::switchContext);
    action->setData(i);

    // don't allow switching to the connected host
    if(host.IsConnected())
      action->setEnabled(false);

    menu->addAction(action);
  }

  if(includeLocalhost)
  {
    QAction *localContext = new QAction(menu);

    localContext->setText(tr("Local"));
    localContext->setIcon(Icons::house());

    QObject::connect(localContext, &QAction::triggered, this, &MainWindow::switchContext);
    localContext->setData(-1);

    menu->addAction(localContext);
  }
}

void MainWindow::setRemoteHost(int hostIdx)
{
  if(!PromptCloseCapture())
    return;

  // we only want to block once before the initial remoteProbe has happened. After that once we can
  // acquire once, we re-release so the next one can happen
  m_RemoteInitialProbeReady.acquire();
  m_RemoteInitialProbeReady.release();

  rdcarray<RemoteHost> hosts = m_Ctx.Config().GetRemoteHosts();

  RemoteHost host;
  if(hostIdx >= 0 && hostIdx < hosts.count())
    host = hosts[hostIdx];

  bool noToAll = false;

  QList<LiveCapture *> liveCaptures = m_LiveCaptures;

  int unsavedCaps = 0;
  for(LiveCapture *live : liveCaptures)
    unsavedCaps += live->unsavedCaptureCount();

  for(LiveCapture *live : liveCaptures)
  {
    // allow live captures to this host to stay open, that way
    // we can connect to a live capture, then switch into that
    // context
    if(host.IsValid() && live->hostname() == host.Hostname())
      continue;

    // if the user previously selected 'no to all' in the save prompts below, apply that to all
    // subsequent live captures by skipping the check and unconditionally cleaning all captures
    if(!noToAll)
    {
      if(!live->checkAllowClose(unsavedCaps, noToAll))
        return;
    }

    live->cleanItems();
    live->close();
  }

  m_Ctx.Replay().DisconnectFromRemoteServer();

  if(!host.IsValid())
  {
    contextChooser->setIcon(Icons::house());
    contextChooser->setText(tr("Replay Context: %1").arg(tr("Local")));

    ui->action_Inject_into_Process->setEnabled(true);

    statusText->setText(QString());

    SetTitle();

    if(m_Ctx.HasCaptureDialog())
      m_Ctx.GetCaptureDialog()->UpdateRemoteHost();
  }
  else
  {
    contextChooser->setText(tr("Replay Context: %1").arg(host.Name()));
    contextChooser->setIcon(host.IsServerRunning() ? Icons::connect() : Icons::disconnect());

    // disable until checking is done
    contextChooser->setEnabled(false);

    ui->action_Inject_into_Process->setEnabled(false);

    SetTitle();

    statusText->setText(tr("Checking remote server status..."));

    LambdaThread *th = new LambdaThread([this, h = host]() {
      // make a mutable copy and see if the server is up
      RemoteHost host = h;
      host.CheckStatus();

      if(host.Protocol() && !host.Protocol()->IsSupported(host.Hostname()))
      {
        // check to see if we should warn the user about this unsupported android version.
        GUIInvoke::call(this, [this, host]() {
          QDateTime today = QDateTime::currentDateTimeUtc();
          QDateTime compare = today.addDays(-21);

          if(host.Protocol()->GetProtocolName() == "adb")
          {
            if(compare > m_Ctx.Config().UnsupportedAndroid_LastUpdate)
            {
              RDDialog::critical(
                  this, tr("Unsupported Device Android Version"),
                  tr("This device is older than Android 6.0, the minimum required version for "
                     "RenderDoc.\n\nThis may break or cause unknown problems - use at your own "
                     "risk."));
            }

            m_Ctx.Config().UnsupportedAndroid_LastUpdate = today;
          }
          else
          {
            RDDialog::critical(
                this, tr("Unsupported Device"),
                tr("This device is not able to support RenderDoc. Please consult the documentation "
                   "for this type of device to see what the problem may be."));
          }
        });
      }

      if(host.Protocol() && host.IsVersionMismatch())
      {
        GUIInvoke::blockcall(this, [this, &host]() {
          QMessageBox::StandardButton res =
              RDDialog::question(this, tr("Unsupported version"),
                                 tr("Remote server on %1 has an incompatible version.\n"
                                    "Would you like to try to reinstall the version %2?")
                                     .arg(host.Name())
                                     .arg(lit(FULL_VERSION_STRING)),
                                 QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

          if(res == QMessageBox::Yes)
          {
            LambdaThread *launchthread = new LambdaThread([&host]() {
              // since we have a protocol, try to force-launch which should attempt to reinstall.
              host.Launch();

              // update status
              host.CheckStatus();
            });
            launchthread->setName(lit("Remote host launch"));
            launchthread->start();

            ShowProgressDialog(this, tr("Attempting to update remote server, please wait..."),
                               [launchthread]() { return !launchthread->isRunning(); });

            launchthread->deleteLater();
          }
        });
      }

      if(!host.IsServerRunning() && !host.RunCommand().isEmpty())
      {
        GUIInvoke::call(this, [this]() {
          statusText->setText(tr("Running remote server command..."));
          statusProgress->setVisible(true);
          statusProgress->setMaximum(0);
        });

        ResultDetails launchResult = host.Launch();
        if(!launchResult.OK())
        {
          showLaunchError(launchResult);
        }

        // check if it's running now
        host.CheckStatus();

        GUIInvoke::call(this, [this]() { statusProgress->setVisible(false); });
      }

      ResultDetails result = {ResultCode::Succeeded};

      if(host.IsServerRunning() && !host.IsBusy())
      {
        result = m_Ctx.Replay().ConnectToRemoteServer(host);
      }

      GUIInvoke::call(this, [this, host, result]() {
        contextChooser->setIcon(host.IsServerRunning() && !host.IsBusy() ? Icons::connect()
                                                                         : Icons::disconnect());

        if(!result.OK())
        {
          contextChooser->setIcon(Icons::cross());
          contextChooser->setText(tr("Replay Context: %1").arg(tr("Local")));
          statusText->setText(tr("Connection failed: %1").arg(result.Message()));
        }
        else if(host.IsVersionMismatch())
        {
          statusText->setText(host.VersionMismatchError());
        }
        else if(host.IsBusy())
        {
          statusText->setText(tr("Remote server in use elsewhere"));
        }
        else if(host.IsServerRunning())
        {
          statusText->setText(tr("Remote server ready"));
        }
        else
        {
          if(!host.RunCommand().isEmpty())
            statusText->setText(tr("Remote server not running or failed to start"));
          else
            statusText->setText(tr("Remote server not running - no start command configured"));
        }

        contextChooser->setEnabled(true);

        if(m_Ctx.HasCaptureDialog())
          m_Ctx.GetCaptureDialog()->UpdateRemoteHost();
      });
    });
    th->setName(lit("Remote host check"));
    th->selfDelete(true);
    th->start();
  }
}

void MainWindow::switchContext()
{
  QAction *item = qobject_cast<QAction *>(QObject::sender());

  if(!item)
    return;

  bool ok = false;
  int hostIdx = item->data().toInt(&ok);

  if(ok)
    setRemoteHost(hostIdx);
}

void MainWindow::contextChooser_menuShowing()
{
  FillRemotesMenu(contextChooserMenu, true);
}

void MainWindow::statusDoubleClicked(QMouseEvent *event)
{
  showDebugMessageView();
}

void MainWindow::OnCaptureLoaded()
{
  // at first only allow the default save for temporary captures. It should be disabled if we have
  // loaded a 'permanent' capture from disk and haven't made any changes. It will be enabled as soon
  // as any changes are made.
  ui->action_Save_Capture_Inplace->setEnabled(m_Ctx.IsCaptureTemporary());
  ui->action_Save_Capture_As->setEnabled(true);
  ui->action_Export_Project->setEnabled(true);
  ui->action_Close_Capture->setEnabled(true);
  ui->menu_Export_As->setEnabled(true);

  // don't allow changing context while capture is open
  contextChooser->setEnabled(false);

  statusProgress->setVisible(false);

  // don't allow capture recompress on opened images
  QString driver = m_Ctx.Replay().GetCaptureAccess()->DriverName();
  bool is_image = driver == lit("Image");
  ui->action_Recompress_Capture->setEnabled(!is_image);

  updateToolsMenuOptions();

  ui->action_Start_Replay_Loop->setEnabled(true);

  ui->action_Open_RGP_Profile->setEnabled(false);
  ui->action_Create_RGP_Profile->setEnabled(m_Ctx.APIProps().rgpCapture && m_Ctx.IsCaptureLocal());
  m_Ctx.Replay().AsyncInvoke([this](IReplayController *) {
    bool hasAMDGRPPorfile =
        (m_Ctx.Replay().GetCaptureAccess()->FindSectionByType(SectionType::AMDRGPProfile) >= 0);

    GUIInvoke::call(this, [this, hasAMDGRPPorfile]() {
      ui->action_Open_RGP_Profile->setEnabled(hasAMDGRPPorfile);
    });
  });

  setCaptureHasErrors(!m_Ctx.DebugMessages().empty());

  ui->action_Resolve_Symbols->setEnabled(false);

  m_Ctx.Replay().AsyncInvoke([this](IReplayController *) {
    bool hasResolver = m_Ctx.Replay().GetCaptureAccess()->HasCallstacks();

    GUIInvoke::call(this, [this, hasResolver]() {
      ui->action_Resolve_Symbols->setEnabled(hasResolver);
      ui->action_Resolve_Symbols->setText(hasResolver ? tr("Resolve Symbols")
                                                      : tr("Resolve Symbols - None in capture"));
    });
  });

  SetTitle();

  PopulateRecentCaptureFiles();

  if(m_Ctx.HasEventBrowser())
    ToolWindowManager::raiseToolWindow(m_Ctx.GetEventBrowser()->Widget());

  // the first time we load a capture with annotations, show/bring the annotation viewer to the
  // front. After that, if the user hides it we won't show it again.
  if(!m_Ctx.Config().Annotations_HasAutoShown && m_Ctx.FrameInfo().containsAnnotations)
  {
    m_Ctx.ShowAnnotationViewer();
    m_Ctx.Config().Annotations_HasAutoShown = true;
    ToolWindowManager::raiseToolWindow(m_Ctx.GetAnnotationViewer()->Widget());
  }
}

void MainWindow::OnCaptureClosed()
{
  ui->action_Save_Capture_Inplace->setEnabled(false);
  ui->action_Save_Capture_As->setEnabled(false);
  ui->action_Export_Project->setEnabled(false);
  ui->action_Close_Capture->setEnabled(false);
  ui->menu_Export_As->setEnabled(false);

  ui->action_Start_Replay_Loop->setEnabled(false);
  ui->action_Open_RGP_Profile->setEnabled(false);
  ui->action_Create_RGP_Profile->setEnabled(false);

  contextChooser->setEnabled(true);

  statusText->setText(QString());
  statusIcon->setPixmap(QPixmap());
  statusProgress->setVisible(false);

  ui->action_Resolve_Symbols->setEnabled(false);
  ui->action_Resolve_Symbols->setText(tr("Resolve Symbols"));

  ui->action_Recompress_Capture->setEnabled(false);
  ui->action_EmbedExternalFiles->setEnabled(false);
  ui->action_RemoveExternalFiles->setEnabled(false);

  SetTitle();

  // if the remote sever disconnected during capture replay, resort back to a 'disconnected' state
  if(m_Ctx.Replay().CurrentRemote().IsValid() && !m_Ctx.Replay().CurrentRemote().IsServerRunning())
  {
    statusText->setText(
        tr("Remote server disconnected. To attempt to reconnect please select it again."));
    contextChooser->setText(tr("Replay Context: %1").arg(tr("Local")));
    m_Ctx.Replay().DisconnectFromRemoteServer();

    if(m_Ctx.HasCaptureDialog())
      m_Ctx.GetCaptureDialog()->UpdateRemoteHost();
  }
}

void MainWindow::OnEventChanged(uint32_t eventId)
{
}

void MainWindow::RegisterShortcut(const rdcstr &shortcut, QWidget *widget, ShortcutCallback callback)
{
  QKeySequence ks = QKeySequence::fromString(shortcut);

  if(widget)
  {
    // we need to create a Qt shortcut for this widget. Even though we don't actually use the
    // callback, unless a shortcut exists Qt might not properly send the ShortcutOverride event to
    // our eventFilter - an example is on windows where Shift-F10 might go straight to a
    // ContextEvent. So we create a shortcut on this widget & key-sequence to force Qt to process it
    m_QtShortcuts.push_back(new QShortcut(ks, widget));

    m_WidgetShortcutCallbacks[ks][widget] = callback;
  }
  else
  {
    if(m_GlobalShortcutCallbacks[ks])
    {
      qCritical() << "Assigning duplicate global shortcut for" << ks;
      return;
    }

    m_GlobalShortcutCallbacks[ks] = callback;
  }
}

void MainWindow::UnregisterShortcut(const rdcstr &shortcut, QWidget *widget)
{
  if(widget)
  {
    // delete any Qt shortcuts we created for this widget
    for(auto it = m_QtShortcuts.begin(); it != m_QtShortcuts.end();)
    {
      if((*it)->parent() == widget)
      {
        delete *it;
        it = m_QtShortcuts.erase(it);
      }
      else
      {
        ++it;
      }
    }

    if(shortcut.isEmpty())
    {
      // if no shortcut is specified, remove all shortcuts for this widget
      for(QMap<QWidget *, ShortcutCallback> &sh : m_WidgetShortcutCallbacks)
        sh.remove(widget);
    }
    else
    {
      QKeySequence ks = QKeySequence::fromString(shortcut);

      m_WidgetShortcutCallbacks[ks].remove(widget);
    }
  }
  else
  {
    QKeySequence ks = QKeySequence::fromString(shortcut);

    m_GlobalShortcutCallbacks.remove(ks);
  }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
  if(event->type() == QEvent::ShortcutOverride)
  {
    QKeyEvent *ke = (QKeyEvent *)event;

    QKeySequence pressed(ke->modifiers() | ke->key());

    // first see if there's a widget shortcut registered for this key. If so, check the focus
    // hierarchy to see if we have any matches
    QWidget *focus = QApplication::focusWidget();

    if(focus && m_WidgetShortcutCallbacks.contains(pressed))
    {
      const QMap<QWidget *, ShortcutCallback> callbacks = m_WidgetShortcutCallbacks[pressed];
      QList<QWidget *> widgets = callbacks.keys();

      while(focus)
      {
        // if we find a direct ancestor to the focus widget which is registered for this shortcut,
        // then use that callback
        if(widgets.contains(focus))
        {
          callbacks[focus](focus);
          event->accept();
          return true;
        }

        // keep searching up the hierarchy
        focus = focus->parentWidget();
      }
    }

    focus = QApplication::focusWidget();

    // if we didn't find matches or no such shortcut is registered, try global shortcuts
    if(m_GlobalShortcutCallbacks.contains(pressed))
    {
      m_GlobalShortcutCallbacks[pressed](focus);
      event->accept();
      return true;
    }
  }

  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::on_action_Close_Capture_triggered()
{
  (void)PromptCloseCapture();
}

void MainWindow::on_action_Save_Capture_Inplace_triggered()
{
  bool saved = false;

  if(m_Ctx.IsCaptureTemporary() || !m_Ctx.IsCaptureLocal())
  {
    saved = PromptSaveCaptureAs();
  }
  else
  {
    if(m_Ctx.GetCaptureModifications() != CaptureModifications::NoModifications &&
       m_Ctx.IsCaptureLocal())
    {
      saved = m_Ctx.SaveCaptureTo(m_Ctx.GetCaptureFilename());
    }
  }

  if(saved)
    ui->action_Save_Capture_Inplace->setEnabled(false);
}

void MainWindow::on_action_Save_Capture_As_triggered()
{
  PromptSaveCaptureAs();
}

void MainWindow::on_action_Export_Project_triggered()
{
  if(!m_Ctx.IsCaptureLoaded())
    return;

  if(!m_Ctx.IsCaptureLocal())
  {
    RDDialog::information(
        this, tr("Save capture locally"),
        tr("The capture is on a remote host, it must be saved locally before the performance "
           "summary can be exported."));

    if(!PromptSaveCaptureAs() || !m_Ctx.IsCaptureLocal())
      return;
  }

  QString outputParent =
      RDDialog::getExistingDirectory(this, tr("Choose performance export folder"),
                                     QFileInfo(m_Ctx.GetCaptureFilename()).absolutePath());

  if(outputParent.isEmpty())
    return;

  QString captureName = QFileInfo(m_Ctx.GetCaptureFilename()).completeBaseName();
  if(captureName.isEmpty())
    captureName = QFileInfo(m_Ctx.GetCaptureFilename()).fileName();
  if(captureName.isEmpty())
    captureName = lit("capture");

  QString outputRoot = QDir(outputParent).absoluteFilePath(
      SanitiseFileComponent(captureName, 40) + lit("_performance_export"));

  if(QDir(outputRoot).exists())
    outputRoot += lit("_") + QDateTime::currentDateTime().toString(lit("yyyyMMdd_HHmmss"));

  ExportPerformanceSummary(this, m_Ctx, outputRoot);
  return;

#if 0
  if(!m_Ctx.IsCaptureLoaded())
    return;

  if(!m_Ctx.IsCaptureLocal())
  {
    RDDialog::information(
        this, tr("Save capture locally"),
        tr("The capture is on a remote host, it must be saved locally before it can be exported."));

    if(!PromptSaveCaptureAs() || !m_Ctx.IsCaptureLocal())
      return;
  }

  QString outputParent =
      RDDialog::getExistingDirectory(this, tr("Choose project export folder"),
                                     QFileInfo(m_Ctx.GetCaptureFilename()).absolutePath());

  if(outputParent.isEmpty())
    return;

  QString captureName = QFileInfo(m_Ctx.GetCaptureFilename()).completeBaseName();
  if(captureName.isEmpty())
    captureName = QFileInfo(m_Ctx.GetCaptureFilename()).fileName();
  if(captureName.isEmpty())
    captureName = lit("capture");

  QString outputRoot = QDir(outputParent).absoluteFilePath(
      SanitiseFileComponent(captureName, 40) + lit("_export_project"));

  if(QDir(outputRoot).exists())
    outputRoot += lit("_") + QDateTime::currentDateTime().toString(lit("yyyyMMdd_HHmmss"));

  QString exportTimestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
  const FrameDescription &frameInfo = m_Ctx.FrameInfo();
  uint32_t frameNumber = frameInfo.frameNumber;
  bool hasFrameNumber = frameNumber != FrameDescription::NoFrameNumber;
  bool containsAnnotations = frameInfo.containsAnnotations;
  QString captureFilename = m_Ctx.GetCaptureFilename();
  QString driverName = FromRDCStr(m_Ctx.Replay().GetCaptureAccess()->DriverName());
  GraphicsAPI apiType = m_Ctx.APIProps().pipelineType;
  QString apiName = ToQStr(apiType);

  QMap<ResourceId, ResourceDescription> resourceDescs;
  QMap<ResourceId, TextureDescription> textureDescs;
  QMap<ResourceId, BufferDescription> bufferDescs;
  QMap<ResourceId, QString> resourceNames;

  for(const ResourceDescription &resource : m_Ctx.GetResources())
  {
    ResourceDescription copy = resource;
    copy.annotations = NULL;
    resourceDescs.insert(copy.resourceId, copy);
    resourceNames.insert(copy.resourceId, m_Ctx.GetResourceName(copy.resourceId));
  }

  for(const TextureDescription &texture : m_Ctx.GetTextures())
    textureDescs.insert(texture.resourceId, texture);

  for(const BufferDescription &buffer : m_Ctx.GetBuffers())
    bufferDescs.insert(buffer.resourceId, buffer);

  QVector<ExportActionInfo> actions;
  CollectExportActions(m_Ctx.CurRootActions(), m_Ctx.GetStructuredFile(), {}, actions);

  uint32_t originalSelectedEvent = m_Ctx.CurSelectedEvent();
  uint32_t originalEvent = m_Ctx.CurEvent();

  std::atomic<bool> finished(false);
  std::atomic<float> progress(0.0f);
  std::atomic<bool> cancelRequested(false);
  bool cancelledByUser = false;
  QString fatalError;
  QVector<ExportActionSummary> summaries;
  int warningCount = 0;
  int totalTextureExports = 0;
  int totalBufferExports = 0;
  int totalMeshExports = 0;
  int totalReferenceExports = 0;

  LambdaThread *exportThread = new LambdaThread([this, outputRoot, exportTimestamp, captureFilename,
                                                 driverName, apiName, apiType, frameNumber,
                                                 hasFrameNumber, containsAnnotations, actions,
                                                 resourceDescs, textureDescs, bufferDescs,
                                                 resourceNames, &finished, &progress,
                                                 &cancelRequested, &cancelledByUser, &fatalError,
                                                 &summaries, &warningCount, &totalTextureExports,
                                                 &totalBufferExports, &totalMeshExports,
                                                 &totalReferenceExports]() mutable {
    auto finish = [&]() { finished.store(true); };
    auto setProgress = [&](int completed, int total) {
      progress.store(total > 0 ? float(completed) / float(total) : 1.0f);
    };

    int totalSteps = qMax(1, textureDescs.count() + bufferDescs.count() + actions.count());
    int completedSteps = 0;

    QDir rootDir(outputRoot);

    if(!QDir().mkpath(rootDir.absolutePath()))
    {
      fatalError = QFormatStr("Couldn't create export directory '%1'.").arg(outputRoot);
      finish();
      return;
    }

    QString actionsRoot = rootDir.absoluteFilePath(lit("actions"));
    if(!QDir().mkpath(actionsRoot))
    {
      fatalError = QFormatStr("Couldn't create export actions directory '%1'.").arg(actionsRoot);
      finish();
      return;
    }

    QMap<uint32_t, QVector<ResourceUsageRef>> usageByEvent;

    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
      for(auto it = textureDescs.begin(); it != textureDescs.end(); ++it)
      {
        if(cancelRequested.load())
        {
          cancelledByUser = true;
          return;
        }

        rdcarray<EventUsage> usages = r->GetUsage(it.key());
        for(const EventUsage &usage : usages)
        {
          ResourceUsageRef ref;
          ref.id = it.key();
          ref.usage = usage.usage;
          ref.eventId = usage.eventId;
          usageByEvent[usage.eventId].push_back(ref);
        }

        completedSteps++;
        setProgress(completedSteps, totalSteps);
      }

      for(auto it = bufferDescs.begin(); it != bufferDescs.end(); ++it)
      {
        if(cancelRequested.load())
        {
          cancelledByUser = true;
          return;
        }

        rdcarray<EventUsage> usages = r->GetUsage(it.key());
        for(const EventUsage &usage : usages)
        {
          ResourceUsageRef ref;
          ref.id = it.key();
          ref.usage = usage.usage;
          ref.eventId = usage.eventId;
          usageByEvent[usage.eventId].push_back(ref);
        }

        completedSteps++;
        setProgress(completedSteps, totalSteps);
      }
    });

    for(const ExportActionInfo &action : actions)
    {
      if(cancelRequested.load())
      {
        cancelledByUser = true;
        break;
      }

      QString actionDirPath = QDir(actionsRoot).absoluteFilePath(action.folderName);
      QString texturesPath = QDir(actionDirPath).absoluteFilePath(lit("textures"));
      QString buffersPath = QDir(actionDirPath).absoluteFilePath(lit("buffers"));
      QString meshPath = QDir(actionDirPath).absoluteFilePath(lit("mesh"));
      QString referencesPath = QDir(actionDirPath).absoluteFilePath(lit("references"));

      if(!QDir().mkpath(texturesPath) || !QDir().mkpath(buffersPath) || !QDir().mkpath(meshPath) ||
         !QDir().mkpath(referencesPath))
      {
        fatalError =
            QFormatStr("Couldn't create export subdirectories for action E%1.").arg(action.eventId);
        break;
      }

      QDir actionDir(actionDirPath);
      QDir texturesDir(texturesPath);
      QDir buffersDir(buffersPath);
      QDir meshDir(meshPath);

      QMap<ResourceId, ResourceAggregate> aggregates;
      auto addResource = [&](ResourceId id, const QString &sourceTag, const QString &usageTag,
                             uint32_t usageEvent) {
        if(id == ResourceId())
          return;

        ResourceAggregate &agg = aggregates[id];
        agg.id = id;
        agg.type = ResourceTypeForId(id, resourceDescs, textureDescs, bufferDescs);
        agg.name = resourceNames.value(id, ResourceIdText(id));

        AppendUnique(agg.sourceTags, sourceTag);
        AppendUnique(agg.usageTags, usageTag);
        AppendUnique(agg.usageEventIds, usageEvent);
      };

      addResource(action.copySource, lit("Action Copy Source"), QString(), 0);
      addResource(action.copyDestination, lit("Action Copy Destination"), QString(), 0);
      addResource(action.depthOut, lit("Action Depth Output"), QString(), 0);

      for(size_t i = 0; i < action.outputs.size(); i++)
        addResource(action.outputs[i], QFormatStr("Action Color Output %1").arg(int(i)), QString(),
                    0);

      for(uint32_t eventId : action.eventIds)
      {
        const QVector<ResourceUsageRef> usageRefs = usageByEvent.value(eventId);
        for(const ResourceUsageRef &usage : usageRefs)
        {
          addResource(usage.id, QFormatStr("Usage at E%1").arg(usage.eventId),
                      ToQStr(usage.usage, apiType), usage.eventId);
        }
      }

      QJsonArray textureMetadata;
      QJsonArray bufferMetadata;
      QJsonArray meshMetadata;
      QJsonArray referenceMetadata;
      QStringList textureLines;
      QStringList bufferLines;
      QStringList meshLines;
      QStringList referenceLines;
      QStringList errorLines;
      int localTextureCount = 0;
      int localBufferCount = 0;
      int localMeshCount = 0;
      int localReferenceCount = 0;
      int localErrorCount = 0;

      m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
        if(cancelRequested.load())
          return;

        r->SetFrameEvent(action.eventId, false);

        const PipeState &pipe = r->GetPipelineState();

        auto addDescriptorResources = [&](const Descriptor &descriptor, const QString &sourceTag) {
          addResource(descriptor.resource, sourceTag, QString(), 0);
          addResource(descriptor.secondary, sourceTag + lit(" Secondary"), QString(), 0);
          addResource(descriptor.view, sourceTag + lit(" View"), QString(), 0);
        };

        auto addUsedDescriptors = [&](const rdcarray<UsedDescriptor> &usedDescriptors,
                                      const QString &stageName, const QString &label) {
          for(const UsedDescriptor &used : usedDescriptors)
          {
            QString sourceTag = QFormatStr("%1 %2").arg(stageName).arg(label);
            addDescriptorResources(used.descriptor, sourceTag);
            addResource(used.sampler.object, sourceTag + lit(" Sampler"), QString(), 0);
            addResource(used.sampler.ycbcrSampler, sourceTag + lit(" YCbCr Conversion"),
                        QString(), 0);
          }
        };

        addResource(pipe.GetGraphicsPipelineObject(), lit("Graphics Pipeline"), QString(), 0);
        addResource(pipe.GetComputePipelineObject(), lit("Compute Pipeline"), QString(), 0);

        for(ShaderStage stage :
            {ShaderStage::Vertex, ShaderStage::Hull, ShaderStage::Domain, ShaderStage::Geometry,
             ShaderStage::Pixel, ShaderStage::Compute, ShaderStage::Task, ShaderStage::Mesh})
        {
          QString stageName = ToQStr(stage, apiType);
          addResource(pipe.GetShader(stage), QFormatStr("%1 Shader").arg(stageName), QString(), 0);
          addUsedDescriptors(pipe.GetConstantBlocks(stage, true), stageName,
                             lit("Constant Buffer"));
          addUsedDescriptors(pipe.GetReadOnlyResources(stage, true), stageName,
                             lit("ReadOnly Resource"));
          addUsedDescriptors(pipe.GetReadWriteResources(stage, true), stageName,
                             lit("ReadWrite Resource"));
          addUsedDescriptors(pipe.GetSamplers(stage, true), stageName, lit("Sampler"));
        }

        addDescriptorResources(pipe.GetDepthTarget(), lit("Depth Target"));
        addDescriptorResources(pipe.GetDepthResolveTarget(), lit("Depth Resolve Target"));

        rdcarray<Descriptor> outputTargets = pipe.GetOutputTargets();
        for(int i = 0; i < outputTargets.count(); i++)
          addDescriptorResources(outputTargets[i], QFormatStr("Color Output %1").arg(i));

        auto exportMeshStage = [&](MeshDataStage stage) {
          MeshFormat mesh = r->GetPostVSData(0, 0, stage);
          QString stageStem = MeshStageFileStem(stage);
          QString stageTitle = ToQStr(stage);

          if(mesh.vertexResourceId == ResourceId() && mesh.indexResourceId == ResourceId() &&
             mesh.numIndices == 0 && FromRDCStr(mesh.status).trimmed().isEmpty())
            return;

          addResource(mesh.vertexResourceId, stageTitle + lit(" Vertex Buffer"), QString(), 0);
          addResource(mesh.indexResourceId, stageTitle + lit(" Index Buffer"), QString(), 0);

          QJsonObject meshObj;
          meshObj[lit("stage")] = stageTitle;
          meshObj[lit("stage_key")] = stageStem;
          meshObj[lit("status")] = FromRDCStr(mesh.status);
          meshObj[lit("topology")] = ToQStr(mesh.topology);
          meshObj[lit("num_indices")] = int(mesh.numIndices);
          meshObj[lit("base_vertex")] = action.baseVertex;
          meshObj[lit("vertex_resource_id")] = ResourceIdText(mesh.vertexResourceId);
          meshObj[lit("vertex_resource_name")] =
              resourceNames.value(mesh.vertexResourceId, ResourceIdText(mesh.vertexResourceId));
          meshObj[lit("vertex_byte_offset")] = ToJsonU64(mesh.vertexByteOffset);
          meshObj[lit("vertex_byte_size")] = ToJsonU64(mesh.vertexByteSize);
          meshObj[lit("vertex_byte_stride")] = int(mesh.vertexByteStride);
          meshObj[lit("index_resource_id")] = ResourceIdText(mesh.indexResourceId);
          meshObj[lit("index_resource_name")] =
              resourceNames.value(mesh.indexResourceId, ResourceIdText(mesh.indexResourceId));
          meshObj[lit("index_byte_offset")] = ToJsonU64(mesh.indexByteOffset);
          meshObj[lit("index_byte_size")] = ToJsonU64(mesh.indexByteSize);
          meshObj[lit("index_byte_stride")] = int(mesh.indexByteStride);
          meshObj[lit("dispatch_size")] = ToJsonArray(mesh.dispatchSize);
          meshObj[lit("meshlet_offset")] = int(mesh.meshletOffset);
          meshObj[lit("meshlet_index_offset")] = int(mesh.meshletIndexOffset);

          QString vertexFile;
          QString indexFile;

          if(mesh.vertexResourceId != ResourceId())
          {
            vertexFile = stageStem + lit("_vertices.bin");
            QString vertexError;
            if(WriteBufferRangeToFile(r, mesh.vertexResourceId, mesh.vertexByteOffset,
                                      mesh.vertexByteSize, meshDir.absoluteFilePath(vertexFile),
                                      vertexError))
            {
              meshObj[lit("vertex_file")] = ToPosixPath(lit("mesh/") + vertexFile);
            }
            else
            {
              meshObj[lit("vertex_error")] = vertexError;
              errorLines.push_back(
                  QFormatStr("- Mesh %1 vertex export failed: %2").arg(stageTitle).arg(
                      vertexError));
              localErrorCount++;
              warningCount++;
            }
          }

          if(mesh.indexResourceId != ResourceId() && mesh.indexByteStride > 0 && mesh.numIndices > 0)
          {
            uint64_t indexSize = mesh.indexByteSize;
            if(indexSize == 0)
              indexSize = uint64_t(mesh.numIndices) * uint64_t(mesh.indexByteStride);

            indexFile = stageStem + lit("_indices.bin");
            QString indexError;
            if(WriteBufferRangeToFile(r, mesh.indexResourceId, mesh.indexByteOffset, indexSize,
                                      meshDir.absoluteFilePath(indexFile), indexError))
            {
              meshObj[lit("index_file")] = ToPosixPath(lit("mesh/") + indexFile);
            }
            else
            {
              meshObj[lit("index_error")] = indexError;
              errorLines.push_back(
                  QFormatStr("- Mesh %1 index export failed: %2").arg(stageTitle).arg(indexError));
              localErrorCount++;
              warningCount++;
            }
          }

          meshMetadata.append(meshObj);
          localMeshCount++;

          QString meshSummary = QFormatStr("- `%1`: topology `%2`, vertices `%3`, indices `%4`")
                                    .arg(stageTitle)
                                    .arg(meshObj[lit("topology")].toString())
                                    .arg(vertexFile.isEmpty() ? lit("-") : vertexFile)
                                    .arg(indexFile.isEmpty() ? lit("-") : indexFile);

          if(!FromRDCStr(mesh.status).trimmed().isEmpty())
            meshSummary += QFormatStr(" (status: %1)").arg(FromRDCStr(mesh.status));

          meshLines.push_back(meshSummary);
        };

        if(action.flags & ActionFlags::Drawcall)
        {
          exportMeshStage(MeshDataStage::VSOut);
          exportMeshStage(MeshDataStage::GSOut);
        }

        if(action.flags & ActionFlags::MeshDispatch)
        {
          exportMeshStage(MeshDataStage::TaskOut);
          exportMeshStage(MeshDataStage::MeshOut);
        }

        for(auto it = aggregates.begin(); it != aggregates.end(); ++it)
        {
          if(cancelRequested.load())
            return;

          const ResourceAggregate &agg = it.value();
          QJsonObject obj = MakeCommonResourceJson(agg, textureDescs, bufferDescs);

          auto texIt = textureDescs.find(agg.id);
          if(texIt != textureDescs.end())
          {
            QString fileName = MakeResourceStem(lit("texture"), agg.id, agg.name) + lit(".dds");
            QString filePath = texturesDir.absoluteFilePath(fileName);

            TextureSave save;
            save.resourceId = agg.id;
            save.typeCast = CompType::Typeless;
            save.destType = FileType::DDS;
            save.mip = -1;
            save.alpha = AlphaMapping::Preserve;

            ResultDetails result = r->SaveTexture(save, filePath);
            if(result.OK())
            {
              obj[lit("output_file")] = ToPosixPath(lit("textures/") + fileName);
              textureLines.push_back(QFormatStr("- `%1`: %2 (%3)")
                                         .arg(fileName)
                                         .arg(agg.name)
                                         .arg(agg.sourceTags.isEmpty()
                                                  ? lit("no source tags")
                                                  : agg.sourceTags.join(lit(", "))));
            }
            else
            {
              obj[lit("error")] = FromRDCStr(result.Message());
              errorLines.push_back(QFormatStr("- Texture %1 export failed: %2").arg(agg.name).arg(
                  FromRDCStr(result.Message())));
              localErrorCount++;
              warningCount++;
            }

            textureMetadata.append(obj);
            localTextureCount++;
            continue;
          }

          auto bufferIt = bufferDescs.find(agg.id);
          if(bufferIt != bufferDescs.end())
          {
            QString fileName = MakeResourceStem(lit("buffer"), agg.id, agg.name) + lit(".bin");
            QString filePath = buffersDir.absoluteFilePath(fileName);
            QString bufferError;

            if(WriteBufferRangeToFile(r, agg.id, 0, bufferIt.value().length, filePath,
                                      bufferError))
            {
              obj[lit("output_file")] = ToPosixPath(lit("buffers/") + fileName);
              bufferLines.push_back(QFormatStr("- `%1`: %2 (%3 bytes)")
                                        .arg(fileName)
                                        .arg(agg.name)
                                        .arg(QString::number(bufferIt.value().length)));
            }
            else
            {
              obj[lit("error")] = bufferError;
              errorLines.push_back(
                  QFormatStr("- Buffer %1 export failed: %2").arg(agg.name).arg(bufferError));
              localErrorCount++;
              warningCount++;
            }

            bufferMetadata.append(obj);
            localBufferCount++;
            continue;
          }

          referenceMetadata.append(obj);
          localReferenceCount++;
          referenceLines.push_back(QFormatStr("- `%1` (%2)")
                                       .arg(agg.name)
                                       .arg(agg.sourceTags.isEmpty() ? lit("metadata only")
                                                                     : agg.sourceTags.join(lit(", "))));
        }
      });

      if(cancelRequested.load())
      {
        cancelledByUser = true;
        break;
      }

      QJsonObject texturesDoc;
      texturesDoc[lit("count")] = localTextureCount;
      texturesDoc[lit("textures")] = textureMetadata;

      QJsonObject buffersDoc;
      buffersDoc[lit("count")] = localBufferCount;
      buffersDoc[lit("buffers")] = bufferMetadata;

      QJsonObject meshDoc;
      meshDoc[lit("count")] = localMeshCount;
      meshDoc[lit("mesh_exports")] = meshMetadata;

      QJsonObject referencesDoc;
      referencesDoc[lit("count")] = localReferenceCount;
      referencesDoc[lit("references")] = referenceMetadata;

      QString writeError;
      if(!WriteJsonFile(actionDir.absoluteFilePath(lit("textures/metadata.json")), texturesDoc,
                        writeError) ||
         !WriteJsonFile(actionDir.absoluteFilePath(lit("buffers/metadata.json")), buffersDoc,
                        writeError) ||
         !WriteJsonFile(actionDir.absoluteFilePath(lit("mesh/metadata.json")), meshDoc, writeError) ||
         !WriteJsonFile(actionDir.absoluteFilePath(lit("references/metadata.json")), referencesDoc,
                        writeError))
      {
        fatalError = writeError;
        break;
      }

      QJsonObject actionDoc;
      actionDoc[lit("action_id")] = int(action.actionId);
      actionDoc[lit("event_id")] = int(action.eventId);
      actionDoc[lit("name")] = action.name;
      actionDoc[lit("hierarchy")] = action.hierarchy;
      actionDoc[lit("kind")] = action.kind;
      actionDoc[lit("flags")] = action.flagsText;
      actionDoc[lit("is_fake_marker")] = action.fakeMarker;
      actionDoc[lit("event_ids")] = ToJsonArray(action.eventIds);
      actionDoc[lit("num_indices")] = int(action.numIndices);
      actionDoc[lit("num_instances")] = int(action.numInstances);
      actionDoc[lit("base_vertex")] = action.baseVertex;
      actionDoc[lit("index_offset")] = int(action.indexOffset);
      actionDoc[lit("vertex_offset")] = int(action.vertexOffset);
      actionDoc[lit("instance_offset")] = int(action.instanceOffset);
      actionDoc[lit("draw_index")] = int(action.drawIndex);
      actionDoc[lit("dispatch_dimension")] = ToJsonArray(action.dispatchDimension);
      actionDoc[lit("dispatch_threads_dimension")] = ToJsonArray(action.dispatchThreadsDimension);
      actionDoc[lit("dispatch_base")] = ToJsonArray(action.dispatchBase);
      actionDoc[lit("copy_source")] = ResourceIdText(action.copySource);
      actionDoc[lit("copy_destination")] = ResourceIdText(action.copyDestination);
      actionDoc[lit("depth_output")] = ResourceIdText(action.depthOut);
      actionDoc[lit("color_outputs")] = ToJsonArray(action.outputs);
      actionDoc[lit("textures_metadata")] = lit("textures/metadata.json");
      actionDoc[lit("buffers_metadata")] = lit("buffers/metadata.json");
      actionDoc[lit("mesh_metadata")] = lit("mesh/metadata.json");
      actionDoc[lit("references_metadata")] = lit("references/metadata.json");
      actionDoc[lit("error_count")] = localErrorCount;

      if(!WriteJsonFile(actionDir.absoluteFilePath(lit("action.json")), actionDoc, writeError))
      {
        fatalError = writeError;
        break;
      }

      QString readme;
      QTextStream ts(&readme);
      ts << "# " << action.name << "\n\n";
      ts << "- Action ID: `" << action.actionId << "`\n";
      ts << "- Event ID: `" << action.eventId << "`\n";
      ts << "- Type: `" << action.kind << "`\n";
      ts << "- Flags: `" << action.flagsText << "`\n";
      ts << "- Hierarchy: `" << (action.hierarchy.isEmpty() ? lit("<root>") : action.hierarchy)
         << "`\n";
      ts << "- Event IDs: `" << JoinUInts(action.eventIds) << "`\n";
      ts << "- Copy source: `" << ResourceIdText(action.copySource) << "`\n";
      ts << "- Copy destination: `" << ResourceIdText(action.copyDestination) << "`\n";
      ts << "- Depth output: `" << ResourceIdText(action.depthOut) << "`\n";
      ts << "- Texture exports: `" << localTextureCount << "`\n";
      ts << "- Buffer exports: `" << localBufferCount << "`\n";
      ts << "- Mesh exports: `" << localMeshCount << "`\n";
      ts << "- Metadata-only references: `" << localReferenceCount << "`\n";
      ts << "- Warnings: `" << localErrorCount << "`\n\n";
      ts << "Metadata files:\n\n";
      ts << "- `action.json`\n";
      ts << "- `textures/metadata.json`\n";
      ts << "- `buffers/metadata.json`\n";
      ts << "- `mesh/metadata.json`\n";
      ts << "- `references/metadata.json`\n\n";

      AppendMarkdownSection(ts, lit("Textures"), textureLines);
      AppendMarkdownSection(ts, lit("Buffers"), bufferLines);
      AppendMarkdownSection(ts, lit("Mesh Exports"), meshLines);
      AppendMarkdownSection(ts, lit("Other References"), referenceLines);
      AppendMarkdownSection(ts, lit("Warnings"), errorLines);

      if(!WriteTextFile(actionDir.absoluteFilePath(lit("README.md")), readme, writeError))
      {
        fatalError = writeError;
        break;
      }

      ExportActionSummary summary;
      summary.action = action;
      summary.relativeReadmePath =
          ToPosixPath(QDir(outputRoot).relativeFilePath(actionDir.absoluteFilePath(lit("README.md"))));
      summary.textureCount = localTextureCount;
      summary.bufferCount = localBufferCount;
      summary.meshCount = localMeshCount;
      summary.referenceCount = localReferenceCount;
      summary.errorCount = localErrorCount;
      summaries.push_back(summary);

      totalTextureExports += localTextureCount;
      totalBufferExports += localBufferCount;
      totalMeshExports += localMeshCount;
      totalReferenceExports += localReferenceCount;

      completedSteps++;
      setProgress(completedSteps, totalSteps);
    }

    if(fatalError.isEmpty())
    {
      QJsonObject manifest;
      manifest[lit("capture_file")] = captureFilename;
      manifest[lit("driver")] = driverName;
      manifest[lit("graphics_api")] = apiName;
      manifest[lit("generated_at")] = exportTimestamp;
      manifest[lit("status")] = cancelledByUser ? lit("cancelled") : lit("completed");
      if(hasFrameNumber)
        manifest[lit("frame_number")] = int(frameNumber);
      manifest[lit("contains_annotations")] = containsAnnotations;
      manifest[lit("action_count")] = int(summaries.count());
      manifest[lit("texture_export_count")] = totalTextureExports;
      manifest[lit("buffer_export_count")] = totalBufferExports;
      manifest[lit("mesh_export_count")] = totalMeshExports;
      manifest[lit("reference_count")] = totalReferenceExports;
      manifest[lit("warning_count")] = warningCount;

      QJsonArray actionsJson;
      for(const ExportActionSummary &summary : summaries)
      {
        QJsonObject obj;
        obj[lit("action_id")] = int(summary.action.actionId);
        obj[lit("event_id")] = int(summary.action.eventId);
        obj[lit("name")] = summary.action.name;
        obj[lit("kind")] = summary.action.kind;
        obj[lit("readme")] = summary.relativeReadmePath;
        obj[lit("texture_count")] = summary.textureCount;
        obj[lit("buffer_count")] = summary.bufferCount;
        obj[lit("mesh_count")] = summary.meshCount;
        obj[lit("reference_count")] = summary.referenceCount;
        obj[lit("warning_count")] = summary.errorCount;
        actionsJson.append(obj);
      }

      manifest[lit("actions")] = actionsJson;

      QString writeError;
      if(!WriteJsonFile(QDir(outputRoot).absoluteFilePath(lit("manifest.json")), manifest,
                        writeError))
      {
        fatalError = writeError;
      }
      else
      {
        QString index;
        QTextStream indexStream(&index);
        indexStream << "# RenderDoc Export Project\n\n";
        indexStream << "- Capture file: `" << captureFilename << "`\n";
        indexStream << "- Driver: `" << driverName << "`\n";
        indexStream << "- Graphics API: `" << apiName << "`\n";
        if(hasFrameNumber)
          indexStream << "- Frame number: `" << frameNumber << "`\n";
        indexStream << "- Generated at: `" << exportTimestamp << "`\n";
        indexStream << "- Status: `" << (cancelledByUser ? lit("cancelled") : lit("completed"))
                    << "`\n";
        indexStream << "- Exported actions: `" << summaries.count() << "`\n";
        indexStream << "- Texture exports: `" << totalTextureExports << "`\n";
        indexStream << "- Buffer exports: `" << totalBufferExports << "`\n";
        indexStream << "- Mesh exports: `" << totalMeshExports << "`\n";
        indexStream << "- Metadata-only references: `" << totalReferenceExports << "`\n";
        indexStream << "- Warnings: `" << warningCount << "`\n\n";
        indexStream << "## Actions\n\n";

        if(summaries.isEmpty())
        {
          indexStream << "- No actions were exported.\n";
        }
        else
        {
          for(const ExportActionSummary &summary : summaries)
          {
            indexStream << "- [`" << summary.relativeReadmePath << "`]("
                        << summary.relativeReadmePath << ")"
                        << " - E" << summary.action.eventId << " - " << summary.action.name
                        << " - textures `" << summary.textureCount << "`, buffers `"
                        << summary.bufferCount << "`, mesh `" << summary.meshCount << "`, refs `"
                        << summary.referenceCount << "`, warnings `" << summary.errorCount
                        << "`\n";
          }
        }

        if(!WriteTextFile(QDir(outputRoot).absoluteFilePath(lit("index.md")), index, writeError))
          fatalError = writeError;
      }
    }

    progress.store(1.0f);
    finish();
  });

  exportThread->setName(lit("Export Project"));
  exportThread->start();

  ShowProgressDialog(this, tr("Exporting project, please wait..."),
                     [&finished]() { return finished.load(); },
                     [&progress]() { return progress.load(); },
                     [&cancelRequested]() { cancelRequested.store(true); });

  exportThread->wait();
  exportThread->deleteLater();

  m_Ctx.SetEventID({}, originalSelectedEvent, originalEvent, true);

  if(!fatalError.isEmpty())
  {
    RDDialog::critical(
        this, tr("Export Project Failed"),
        tr("Project export failed. Partial output may exist in:\n%1\n\n%2")
            .arg(outputRoot)
            .arg(fatalError));
  }
  else if(cancelledByUser)
  {
    RDDialog::information(this, tr("Export Project Cancelled"),
                          tr("Project export was cancelled. Partial output was written to:\n%1")
                              .arg(outputRoot));
  }
  else if(warningCount > 0)
  {
    RDDialog::information(
        this, tr("Export Project Complete"),
        tr("Project export completed with %1 warnings.\n\nOutput:\n%2")
            .arg(warningCount)
            .arg(outputRoot));
  }
  else
  {
    RDDialog::information(this, tr("Export Project Complete"),
                          tr("Project export completed successfully.\n\nOutput:\n%1")
                              .arg(outputRoot));
  }
#endif
}

void MainWindow::on_action_About_triggered()
{
  AboutDialog about(this);
  RDDialog::show(&about);
}

void MainWindow::on_action_Mesh_Output_triggered()
{
  QWidget *meshPreview = m_Ctx.GetMeshPreview()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(meshPreview))
    ToolWindowManager::raiseToolWindow(meshPreview);
  else
    ui->toolWindowManager->addToolWindow(meshPreview, mainToolArea());
}

void MainWindow::on_action_API_Inspector_triggered()
{
  QWidget *apiInspector = m_Ctx.GetAPIInspector()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(apiInspector))
  {
    ToolWindowManager::raiseToolWindow(apiInspector);
  }
  else
  {
    if(m_Ctx.HasEventBrowser() &&
       ui->toolWindowManager->toolWindows().contains(m_Ctx.GetEventBrowser()->Widget()))
    {
      ToolWindowManager::AreaReference ref(
          ToolWindowManager::BottomOf,
          ui->toolWindowManager->areaOf(m_Ctx.GetEventBrowser()->Widget()));
      ui->toolWindowManager->addToolWindow(apiInspector, ref);
    }
    else
    {
      ui->toolWindowManager->addToolWindow(apiInspector, leftToolArea());
    }
  }
}

void MainWindow::on_action_Annotation_Viewer_triggered()
{
  QWidget *annotViewer = m_Ctx.GetAnnotationViewer()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(annotViewer))
  {
    ToolWindowManager::raiseToolWindow(annotViewer);
  }
  else
  {
    if(m_Ctx.HasAPIInspector() &&
       ui->toolWindowManager->toolWindows().contains(m_Ctx.GetAPIInspector()->Widget()))
    {
      ToolWindowManager::AreaReference ref(
          ToolWindowManager::AddTo, ui->toolWindowManager->areaOf(m_Ctx.GetAPIInspector()->Widget()));
      ui->toolWindowManager->addToolWindow(annotViewer, ref);
    }
    else if(m_Ctx.HasEventBrowser() &&
            ui->toolWindowManager->toolWindows().contains(m_Ctx.GetEventBrowser()->Widget()))
    {
      ToolWindowManager::AreaReference ref(
          ToolWindowManager::BottomOf,
          ui->toolWindowManager->areaOf(m_Ctx.GetEventBrowser()->Widget()));
      ui->toolWindowManager->addToolWindow(annotViewer, ref);
    }
    else
    {
      ui->toolWindowManager->addToolWindow(annotViewer, leftToolArea());
    }
  }
}

void MainWindow::on_action_Event_Browser_triggered()
{
  QWidget *eventBrowser = m_Ctx.GetEventBrowser()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(eventBrowser))
    ToolWindowManager::raiseToolWindow(eventBrowser);
  else
    ui->toolWindowManager->addToolWindow(eventBrowser, leftToolArea());
}

void MainWindow::on_action_Texture_Viewer_triggered()
{
  QWidget *textureViewer = m_Ctx.GetTextureViewer()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(textureViewer))
    ToolWindowManager::raiseToolWindow(textureViewer);
  else
    ui->toolWindowManager->addToolWindow(textureViewer, mainToolArea());
}

void MainWindow::on_action_Pipeline_State_triggered()
{
  QWidget *pipelineViewer = m_Ctx.GetPipelineViewer()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(pipelineViewer))
    ToolWindowManager::raiseToolWindow(pipelineViewer);
  else
    ui->toolWindowManager->addToolWindow(pipelineViewer, mainToolArea());
}

void MainWindow::on_action_Launch_Application_triggered()
{
  ICaptureDialog *capDialog = m_Ctx.GetCaptureDialog();

  capDialog->SetInjectMode(false);

  if(ui->toolWindowManager->toolWindows().contains(capDialog->Widget()))
    ToolWindowManager::raiseToolWindow(capDialog->Widget());
  else
    ui->toolWindowManager->addToolWindow(capDialog->Widget(), mainToolArea());
}

void MainWindow::on_action_Inject_into_Process_triggered()
{
  ICaptureDialog *capDialog = m_Ctx.GetCaptureDialog();

  capDialog->SetInjectMode(true);

  if(ui->toolWindowManager->toolWindows().contains(capDialog->Widget()))
    ToolWindowManager::raiseToolWindow(capDialog->Widget());
  else
    ui->toolWindowManager->addToolWindow(capDialog->Widget(), mainToolArea());
}

void MainWindow::on_action_Errors_and_Warnings_triggered()
{
  QWidget *debugMessages = m_Ctx.GetDebugMessageView()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(debugMessages))
    ToolWindowManager::raiseToolWindow(debugMessages);
  else
    ui->toolWindowManager->addToolWindow(debugMessages, mainToolArea());
}

void MainWindow::on_action_Comments_triggered()
{
  QWidget *comments = m_Ctx.GetCommentView()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(comments))
    ToolWindowManager::raiseToolWindow(comments);
  else
    ui->toolWindowManager->addToolWindow(comments, mainToolArea());
}

void MainWindow::on_action_Statistics_Viewer_triggered()
{
  QWidget *stats = m_Ctx.GetStatisticsViewer()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(stats))
    ToolWindowManager::raiseToolWindow(stats);
  else
    ui->toolWindowManager->addToolWindow(stats, mainToolArea());
}

void MainWindow::on_action_Timeline_triggered()
{
  QWidget *stats = m_Ctx.GetTimelineBar()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(stats))
    ToolWindowManager::raiseToolWindow(stats);
  else
    ui->toolWindowManager->addToolWindow(
        stats,
        ToolWindowManager::AreaReference(ToolWindowManager::TopWindowSide, mainToolArea().area()));
}

void MainWindow::on_action_Python_Shell_triggered()
{
  QWidget *py = m_Ctx.GetPythonShell()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(py))
    ToolWindowManager::raiseToolWindow(py);
  else
    ui->toolWindowManager->addToolWindow(py, mainToolArea());
}

void MainWindow::on_action_Resolve_Symbols_triggered()
{
  ANALYTIC_SET(UIFeatures.CallstackResolve, true);

  if(!m_Ctx.Replay().GetCaptureAccess())
  {
    RDDialog::critical(
        this, tr("Not Available"),
        tr("Callstack resolution is not available.\n\nCheck remote server connection."));
    return;
  }

  float progress = 0.0f;
  bool finished = false;

  m_Ctx.Replay().AsyncInvoke([this, &progress, &finished](IReplayController *) {
    ResultDetails success = m_Ctx.Replay().GetCaptureAccess()->InitResolver(
        true, [&progress](float p) { progress = p; });

    if(!success.OK())
    {
      RDDialog::critical(
          this, tr("Error loading symbols"),
          tr("Couldn't load symbols for callstack resolution.\n\n%1").arg(success.Message()));
    }

    finished = true;
  });

  ShowProgressDialog(
      this, tr("Resolving symbols, please wait..."), [&finished]() { return finished; },
      [&progress]() { return progress; });

  if(m_Ctx.HasAPIInspector())
    m_Ctx.GetAPIInspector()->Refresh();
}

void MainWindow::on_action_Recompress_Capture_triggered()
{
  m_Ctx.RecompressCapture();
}

void MainWindow::on_action_EmbedExternalFiles_triggered()
{
  m_Ctx.EmbedDependentFiles();
}

void MainWindow::on_action_RemoveExternalFiles_triggered()
{
  m_Ctx.RemoveDependentFiles();
}

void MainWindow::on_action_Start_Replay_Loop_triggered()
{
  if(!m_Ctx.IsCaptureLoaded())
    return;

  RDDialog popup;
  popup.setWindowFlags(popup.windowFlags() & ~Qt::WindowContextHelpButtonHint);
  popup.setWindowIcon(windowIcon());

  const TextureDescription *displayTex = NULL;

  const ActionDescription *lastAction = m_Ctx.GetLastAction();

  displayTex = m_Ctx.GetTexture(lastAction->copyDestination);
  if(!displayTex)
    displayTex = m_Ctx.GetTexture(lastAction->outputs[0]);

  if(!displayTex)
  {
    // if no texture was bound, then use the first colour swapbuffer
    for(const TextureDescription &tex : m_Ctx.GetTextures())
    {
      if((tex.creationFlags & TextureCategory::SwapBuffer) &&
         tex.format.compType != CompType::Depth && tex.format.type != ResourceFormatType::D16S8 &&
         tex.format.type != ResourceFormatType::D24S8 && tex.format.type != ResourceFormatType::D32S8)
      {
        displayTex = &tex;
        break;
      }
    }
  }

  if(!displayTex)
  {
    // if still no texture was found, then use the biggest colour render target
    for(const TextureDescription &tex : m_Ctx.GetTextures())
    {
      if((tex.creationFlags & TextureCategory::ColorTarget) &&
         tex.format.compType != CompType::Depth && tex.format.type != ResourceFormatType::D16S8 &&
         tex.format.type != ResourceFormatType::D24S8 && tex.format.type != ResourceFormatType::D32S8)
      {
        if(displayTex == NULL || tex.width * tex.height > displayTex->width * displayTex->height)
          displayTex = &tex;
      }
    }
  }

  ResourceId id;

  if(displayTex)
  {
    id = displayTex->resourceId;
    popup.resize((int)displayTex->width, (int)displayTex->height);
    popup.setWindowTitle(tr("Looping replay of %1 Displaying %2")
                             .arg(m_Ctx.GetCaptureFilename())
                             .arg(m_Ctx.GetResourceName(id)));
  }
  else
  {
    popup.resize(100, 100);
    popup.setWindowTitle(
        tr("Looping replay of %1 Displaying %2").arg(m_Ctx.GetCaptureFilename()).arg(tr("nothing")));
  }

  WindowingData winData = m_Ctx.CreateWindowingData(&popup);

  m_Ctx.Replay().AsyncInvoke([winData, id](IReplayController *r) { r->ReplayLoop(winData, id); });

  QObject::connect(&popup, &RDDialog::aboutToClose,
                   [this](QCloseEvent *) { m_Ctx.Replay().CancelReplayLoop(); });
  QObject::connect(&popup, &RDDialog::keyPress, [this](QKeyEvent *e) {
    if(e->matches(QKeySequence::Cancel))
      m_Ctx.Replay().CancelReplayLoop();
  });

  RDDialog::show(&popup);

  m_Ctx.Replay().CancelReplayLoop();
}

void MainWindow::on_action_Open_RGP_Profile_triggered()
{
  if(!m_Ctx.IsCaptureLoaded())
    return;

  int idx = m_Ctx.Replay().GetCaptureAccess()->FindSectionByType(SectionType::AMDRGPProfile);

  if(idx < 0)
    return;

  QString path = QDir::temp().absoluteFilePath(lit("renderdoc_extracted.rgp"));

  QFile f(path);
  if(f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    bytebuf buf = m_Ctx.Replay().GetCaptureAccess()->GetSectionContents(idx);

    f.write((const char *)buf.data(), (qint64)buf.size());
    f.flush();
  }
  else
  {
    qCritical() << "Couldn't open temporary file " << path << " for write";
    return;
  }

  m_Ctx.OpenRGPProfile(path);
}

void MainWindow::on_action_Create_RGP_Profile_triggered()
{
  if(!m_Ctx.IsCaptureLoaded())
    return;

  if(m_Ctx.Replay().GetCaptureAccess()->FindSectionByType(SectionType::AMDRGPProfile) >= 0)
  {
    QMessageBox::StandardButton res = RDDialog::question(
        this, tr("Existing RGP profile"), tr("Capture already contains an RGP profile. Overwrite?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

    if(res != QMessageBox::Yes)
      return;
  }

  QDialog popup;
  popup.setWindowFlags(popup.windowFlags() & ~Qt::WindowContextHelpButtonHint);
  popup.setWindowIcon(windowIcon());
  popup.resize(128, 16);
  popup.setWindowTitle(tr("Making RGP Profile from %1").arg(m_Ctx.GetCaptureFilename()));

  WindowingData winData = m_Ctx.CreateWindowingData(&popup);

  rdcstr path;

  m_Ctx.Replay().AsyncInvoke([this, winData, &popup, &path](IReplayController *r) {
    path = r->CreateRGPProfile(winData);
    GUIInvoke::call(this, [&popup]() { popup.close(); });
  });

  RDDialog::show(&popup);

  qInfo() << "RGP Capture created at" << QString(path);
  m_Ctx.OpenRGPProfile(path);

  if(!path.isEmpty())
  {
    QFile f(path);
    if(f.open(QIODevice::ReadOnly))
    {
      QByteArray contents = f.readAll();

      bytebuf buf;
      buf.resize(contents.count());
      memcpy(&buf[0], contents.data(), contents.count());

      SectionProperties props;
      props.type = SectionType::AMDRGPProfile;
      props.version = 1;
      props.flags = SectionFlags::ZstdCompressed;

      m_Ctx.Replay().GetCaptureAccess()->WriteSection(props, buf);

      ui->action_Open_RGP_Profile->setEnabled(true);
    }
    else
    {
      qCritical() << "Couldn't read from temporary RGP capture at " << QString(path);
    }
  }
}

void MainWindow::on_action_Attach_to_Running_Instance_triggered()
{
  on_action_Manage_Remote_Servers_triggered();
}

void MainWindow::on_action_Manage_Extensions_triggered()
{
  ExtensionManager manager(m_Ctx);
  RDDialog::show(&manager);
}

void MainWindow::on_action_Manage_Remote_Servers_triggered()
{
  LambdaThread *th = new LambdaThread([this]() {
    m_Ctx.Config().UpdateEnumeratedProtocolDevices();

    GUIInvoke::call(this, [this]() {
      RemoteManager *rm = new RemoteManager(m_Ctx, this);
      RDDialog::show(rm);
      // now that we're done with it, the manager deletes itself when all lookups terminate (or
      // immediately if there are no lookups ongoing).
      rm->closeWhenFinished();
    });
  });
  th->start();
  th->wait(500);
  if(th->isRunning())
  {
    ShowProgressDialog(this, tr("Updating available devices, please wait..."),
                       [th]() { return !th->isRunning(); });
  }
  th->deleteLater();
}

void MainWindow::on_action_Settings_triggered()
{
  SettingsDialog about(m_Ctx, this);
  RDDialog::show(&about);
}

void MainWindow::on_action_View_Documentation_triggered()
{
  QFileInfo fi(QGuiApplication::applicationFilePath());

  if(fi.absoluteDir().exists(lit("renderdoc.chm")))
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(fi.absoluteDir().absoluteFilePath(lit("renderdoc.chm"))));
  else
    QDesktopServices::openUrl(QUrl::fromUserInput(lit("https://renderdoc.org/docs")));
}

void MainWindow::on_action_Source_on_GitHub_triggered()
{
  QDesktopServices::openUrl(QUrl::fromUserInput(lit("https://github.com/baldurk/renderdoc")));
}

void MainWindow::on_action_Build_Release_Downloads_triggered()
{
  QDesktopServices::openUrl(QUrl::fromUserInput(lit("https://renderdoc.org/builds")));
}

void MainWindow::on_action_Show_Tips_triggered()
{
  TipsDialog tipsDialog(m_Ctx, this);
  RDDialog::show(&tipsDialog);
}

void MainWindow::on_action_Counter_Viewer_triggered()
{
  QWidget *performanceCounterViewer = m_Ctx.GetPerformanceCounterViewer()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(performanceCounterViewer))
    ToolWindowManager::raiseToolWindow(performanceCounterViewer);
  else
    ui->toolWindowManager->addToolWindow(performanceCounterViewer, mainToolArea());
}

void MainWindow::on_action_Resource_Inspector_triggered()
{
  QWidget *resourceInspector = m_Ctx.GetResourceInspector()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(resourceInspector))
    ToolWindowManager::raiseToolWindow(resourceInspector);
  else
    ui->toolWindowManager->addToolWindow(resourceInspector, mainToolArea());
}

void MainWindow::on_action_Send_Error_Report_triggered()
{
  sendErrorReport(false);
}

void MainWindow::sendErrorReport(bool forceCaptureInclusion)
{
  if(!ErrorReportsAllowed())
    return;

  rdcstr report;
  RENDERDOC_CreateBugReport(RENDERDOC_GetLogFile(), "", report);

  QVariantMap json;

  json[lit("version")] = lit(FULL_VERSION_STRING);
  json[lit("gitcommit")] = QString::fromLatin1(RENDERDOC_GetCommitHash());
  json[lit("replaycrash")] = 1;
  json[lit("manual")] = 1;
  json[lit("forcecapture")] = forceCaptureInclusion ? 1 : 0;
  json[lit("report")] = (QString)report;

  CrashDialog crash(m_Ctx.Config(), json, this);

  RDDialog::show(&crash);

  m_Ctx.Config().Save();
  PopulateReportedBugs();

  QFile::remove(QString(report));
}

void MainWindow::on_action_Check_for_Updates_triggered()
{
  CheckUpdates(true, [this](UpdateResult updateResult) {
    switch(updateResult)
    {
      case UpdateResult::Disabled:
      case UpdateResult::Toosoon:
      {
        // won't happen, we forced the check
        break;
      }
      case UpdateResult::Unofficial:
      {
        QMessageBox::StandardButton res =
            RDDialog::question(this, tr("Unofficial build"),
                               tr("You are running an unofficial build, not a stable release.\n"
                                  "Updates are only available for installed release builds\n\n"
                                  "Would you like to open the builds list in a browser?"));

        if(res == QMessageBox::Yes)
          QDesktopServices::openUrl(lit("https://renderdoc.org/builds"));
        break;
      }
      case UpdateResult::Latest:
      {
        RDDialog::information(this, tr("Latest version"), tr("You are running the latest version."));
        break;
      }
      case UpdateResult::Upgrade:
      {
        // CheckUpdates() will have shown a dialog for this
        break;
      }
    }
  });
}

void MainWindow::showDiagnosticLogView()
{
  QWidget *logView = m_Ctx.GetDiagnosticLogView()->Widget();

  if(ui->toolWindowManager->toolWindows().contains(logView))
    ToolWindowManager::raiseToolWindow(logView);
  else
    ui->toolWindowManager->addToolWindow(logView, mainToolArea());
}

void MainWindow::updateAvailable_triggered()
{
  bool mismatch = HandleMismatchedVersions();
  if(mismatch)
    return;

  SetUpdateAvailable();
  UpdatePopup();
}

void MainWindow::saveLayout_triggered()
{
  LoadSaveLayout(qobject_cast<QAction *>(QObject::sender()), true);
}

void MainWindow::loadLayout_triggered()
{
  LoadSaveLayout(qobject_cast<QAction *>(QObject::sender()), false);
}

void MainWindow::updateToolsMenuOptions()
{
  if(m_Ctx.Replay().GetCaptureAccess())
  {
    m_Ctx.Replay().AsyncInvoke([this](IReplayController *) {
      bool hasEmbeddedDependencies = m_Ctx.Replay().GetCaptureAccess()->HasEmbeddedDependencies();
      bool hasPendingDependencies = m_Ctx.Replay().GetCaptureAccess()->HasPendingDependencies();

      GUIInvoke::call(this, [this, hasEmbeddedDependencies, hasPendingDependencies]() {
        ui->action_EmbedExternalFiles->setEnabled(!hasEmbeddedDependencies && hasPendingDependencies);
        ui->action_RemoveExternalFiles->setEnabled(hasEmbeddedDependencies);
      });
    });
  }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
  if(RENDERDOC_IsGlobalHookActive())
  {
    RDDialog::critical(this, tr("Global hook active"),
                       tr("Cannot close RenderDoc while global hook is active."));
    event->ignore();
    return;
  }

  if(!PromptCloseCapture())
  {
    event->ignore();
    return;
  }

  bool noToAll = false;

  QList<QPointer<LiveCapture>> liveCaptures;

  int unsavedCaps = 0;
  for(QPointer<LiveCapture> live : m_LiveCaptures)
  {
    unsavedCaps += live->unsavedCaptureCount();
    liveCaptures.append(live);
  }

  for(QPointer<LiveCapture> live : liveCaptures)
  {
    // The live capture could be deleted during this loop via the save capture modal message box
    // message pump of an earlier live capture
    if(live.isNull())
      continue;

    // if the user previously selected 'no to all' in the save prompts below, apply that to all
    // subsequent live captures by skipping the check and unconditionally cleaning all captures
    if(!noToAll)
    {
      if(!live->checkAllowClose(unsavedCaps, noToAll))
      {
        event->ignore();
        return;
      }
    }

    live->cleanItems();
    delete live;
  }

  SaveLayout(0);
}

void MainWindow::changeEvent(QEvent *event)
{
  if(event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
    QPixmapCache::clear();
}

QString MainWindow::dragFilename(const QMimeData *mimeData)
{
  if(mimeData->hasUrls())
  {
    QList<QUrl> urls = mimeData->urls();
    if(urls.size() == 1 && urls[0].isLocalFile())
    {
      QFileInfo f(urls[0].toLocalFile());
      if(f.exists())
        return f.absoluteFilePath();
    }
  }

  return QString();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
  if(!dragFilename(event->mimeData()).isEmpty())
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
  QString fn = dragFilename(event->mimeData());
  if(!fn.isEmpty())
  {
    // we defer this so we can return immediately and unblock whichever application dropped the
    // item.
    GUIInvoke::defer(this, [this, fn]() { LoadFromFilename(fn, false); });
  }
}

void MainWindow::LoadSaveLayout(QAction *action, bool save)
{
  if(action == NULL)
  {
    qWarning() << "NULL action passed to LoadSaveLayout - bad signal?";
    return;
  }

  bool success = false;

  if(action == ui->action_Save_Default_Layout)
  {
    success = SaveLayout(0);
  }
  else if(action == ui->action_Load_Default_Layout)
  {
    success = LoadLayout(0);
  }
  else
  {
    QString name = action->objectName();
    name.remove(0, name.size() - 1);
    int idx = name.toInt();

    if(idx > 0)
    {
      if(save)
        success = SaveLayout(idx);
      else
        success = LoadLayout(idx);
    }
  }

  if(!success)
  {
    if(save)
      RDDialog::critical(this, tr("Error saving layout"), tr("Couldn't save layout"));
    else
      RDDialog::critical(this, tr("Error loading layout"), tr("Couldn't load layout"));
  }
}

QVariantMap MainWindow::saveState()
{
  QVariantMap state = ui->toolWindowManager->saveState();

  state[lit("mainWindowGeometry")] = QString::fromLatin1(saveGeometry().toBase64());

  return state;
}

bool MainWindow::restoreState(QVariantMap &state)
{
  restoreGeometry(QByteArray::fromBase64(state[lit("mainWindowGeometry")].toByteArray()));

  ui->toolWindowManager->restoreState(state);

  return true;
}

bool MainWindow::SaveLayout(int layout)
{
  qInfo() << "SaveLayout " << layout;
  QString path = GetLayoutPath(layout);

  QVariantMap state = saveState();

  QFile f(path);
  if(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    return SaveToJSON(state, f, JSON_ID, JSON_VER);

  qWarning() << "Couldn't write to " << path << " " << f.errorString();

  return false;
}

bool MainWindow::LoadLayout(int layout)
{
  qInfo() << "LoadLayout " << layout;
  QString path = GetLayoutPath(layout);

  QFile f(path);
  if(f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    QVariantMap state;

    bool success = LoadFromJSON(state, f, JSON_ID, JSON_VER);

    if(!success)
      return false;

    if(restoreState(state))
    {
      // Close any windows which have now become orphaned
      foreach(QWidget *toolWindow, ui->toolWindowManager->toolWindows())
      {
        if(ui->toolWindowManager->areaOf(toolWindow) == NULL)
        {
          qInfo() << "Manually closing orphaned window " << toolWindow->objectName();
          ui->toolWindowManager->forceCloseToolWindow(toolWindow);
        }
      }
      return true;
    }
    return false;
  }

  qInfo() << "Couldn't load layout from " << path << " " << f.errorString();

  return false;
}

void MainWindow::showLaunchError(ResultDetails result)
{
  QString message;
  switch(result.code)
  {
    case ResultCode::AndroidGrantPermissionsFailed:
      message =
          tr("%1.\n\n"
             "Please manually allow the RenderDocCmd program storage permissions on your device "
             "to ensure correct functionality.")
              .arg(result.Message());
      break;
    case ResultCode::AndroidABINotFound:
      message = tr("%1.\n\nPlease check device connection and result.").arg(result.Message());
      break;
    case ResultCode::AndroidAPKFolderNotFound: message = result.Message(); break;
    case ResultCode::AndroidAPKInstallFailed:
      message = tr("%1.\n\nPlease check that your device is connected and accessible to "
                   "adb, and that installing APKs over USB is allowed.")
                    .arg(result.Message());
      break;
    case ResultCode::AndroidAPKVerifyFailed:
      message =
          tr("Couldn't correctly verify installed APK version.\n\n"
             "Please check your installation is not corrupted."
#if !RENDERDOC_OFFICIAL_BUILD
             " Or if this is a custom build check that all ABIs are built at the same version as "
             "this program."
#endif
          );
      break;
    default:
      message = tr("Error encountered launching RenderDoc remote server: %1.").arg(result.Message());
      break;
  }
  GUIInvoke::call(this, [this, message]() {
    RDDialog::warning(this, tr("Problems launching RenderDoc remote server"), message);
  });
}

bool MainWindow::isUnshareableDeviceInUse()
{
  if(!m_Ctx.Replay().CurrentRemote().Protocol())
    return false;

  rdcstr host = m_Ctx.Replay().CurrentRemote().Hostname();

  if(m_Ctx.Replay().CurrentRemote().Protocol()->SupportsMultiplePrograms(host))
    return false;

  uint32_t ident = RENDERDOC_EnumerateRemoteTargets(host, 0);
  return ident != 0;
}
