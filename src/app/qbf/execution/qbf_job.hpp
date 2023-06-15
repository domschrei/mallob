
#pragma once

#include <stdlib.h>
#include "app/job.hpp"
#include "app/qbf/execution/qbf_context.hpp"
#include "app/qbf/execution/bloqqer_caller.hpp"
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

    JobResult _internal_result;

    Mutex _mtx_msg_queue;
    std::list<MessageHandle> _msg_queue;

    BloqqerCaller _bloqqerCaller;

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
    using Payload = std::vector<int>;
    std::pair<ChildJobApp, std::vector<Payload>> applySplittingStrategy(QbfContext& ctx);
    void spawnChildJob(QbfContext& ctx, ChildJobApp app, int childIdx, Payload&& formula);

    void markDone(int resultCode = 0);

    AppConfiguration getAppConfig();
    nlohmann::json getJobSubmissionJson(ChildJobApp app, const AppConfiguration& appConfig);

    static void onJobReadyNotification(MessageHandle& h, const QbfContext& submitCtx);
    static void onJobCancelled(MessageHandle& h, const QbfContext& submitCtx);
    void onResultNotification(MessageHandle& h, const QbfContext& submitCtx);
    void onSatJobDone(const nlohmann::json& response, QbfContext& ctx);

    void handleSubjobDone(int nodeJobId, QbfNotification& msg);
};
