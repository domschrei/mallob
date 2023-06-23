
#pragma once

#include <stdlib.h>
#include "app/job.hpp"
#include "app/qbf/execution/qbf_context.hpp"
#include "comm/msg_queue/message_handle.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/msgtags.h"
#include "core/client.hpp"
#include "data/app_configuration.hpp"
#include "data/permanent_cache.hpp"
#include "util/str_util.hpp"
#include "util/sys/background_worker.hpp"

class QbfJob : public Job {

private:
    Logger _job_log;

    bool _initialized {false};
    bool _sent_ready_msg_to_parent {false};
    bool _bg_worker_done {false};
    BackgroundWorker _bg_worker;

    Mutex _mtx_app_config;

    int _internal_job_counter {1};
    int _internal_result_code {0};

    JobResult _internal_result;

    Mutex _mtx_msg_queue;
    std::list<MessageHandle> _msg_queue;

public:
    QbfJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    void appl_start() override;
    void appl_suspend() override;
    void appl_resume() override;
    void appl_terminate() override;
    int appl_solved() override;
    JobResult&& appl_getResult() override ;
    void appl_communicate() override;
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override;
    void appl_dumpStats() override;
    bool appl_isDestructible() override;
    void appl_memoryPanic() override;
    virtual ~QbfJob();

    virtual int getDemand() const override;

private:
    void run();

    std::pair<size_t, const int*> getFormulaWithQuantifications();
    size_t getNumQuantifications(size_t fSize, const int* fData);

    QbfContext buildQbfContextFromAppConfig();
    void installMessageListeners(QbfContext& submitCtx);

    enum ChildJobApp {QBF, SAT};
    using Formula = std::vector<int>;

    std::optional<std::pair<ChildJobApp, std::vector<std::pair<int, Formula>>>> applySplittingStrategy();
    std::optional<std::pair<ChildJobApp, std::vector<std::pair<int, Formula>>>> applyTrivialSplittingStrategy();
    std::optional<std::pair<ChildJobApp, std::vector<std::pair<int, Formula>>>> applyIterativeDeepeningSplittingStrategy();

    std::vector<std::pair<int, Formula>> prepareSatChildJobs(QbfContext& ctx, const int* begin, const int* end, int vars);
    std::vector<std::pair<int, Formula>> prepareQbfChildJobs(QbfContext& ctx, const int* begin, const int* end, int varToSplitOn, int vars);

    void spawnChildJob(QbfContext& ctx, ChildJobApp app, int childIdx, Formula&& formula, int vars);

    void reportJobDone(QbfContext& ctx, int resultCode);
    void markDone(QbfContext& ctx, bool jobAlive, int resultCode = -1);
    void eraseContextAndConclude(int nodeJobId);

    AppConfiguration getAppConfig();
    nlohmann::json getJobSubmissionJson(ChildJobApp app, const AppConfiguration& appConfig);

    void onJobReadyNotification(MessageHandle& h, const QbfContext& submitCtx);
    void onJobCancelled(MessageHandle& h, const QbfContext& submitCtx);
    void onResultNotification(MessageHandle& h, const QbfContext& submitCtx);
    void onSatJobDone(const nlohmann::json& response, QbfContext& ctx);

    void handleSubjobDone(int nodeJobId, QbfNotification& msg);

    static size_t findIdxOfFirstZero(const int* data, size_t size);
};
