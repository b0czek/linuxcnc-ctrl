#include <google/protobuf/empty.pb.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "grpc/server/deferred_write_finish.hpp"
#include "grpc/server/service_factories.hpp"
#include "grpc/server/unary_task_reactor.hpp"
#include "linuxcnc/v1/machine.grpc.pb.h"
#include "linuxcnc_grpc/callback_runtime.hpp"
#include "linuxcnc_grpc/command_coordinator.hpp"
#include "linuxcnc_grpc/daemon/config.hpp"
#include "linuxcnc_grpc/linuxcnc/command_validation.hpp"
#include "linuxcnc_grpc/linuxcnc/nml_adapter.hpp"
#include "linuxcnc_grpc/position/telemetry.hpp"
#include "linuxcnc_grpc/program/workspace.hpp"
#include "machine/grpc/status_codec.hpp"

namespace linuxcnc::server::detail {
namespace {

using namespace ::linuxcnc::v1;

constexpr std::uint32_t kMaxPositionSamplePeriodMs = 60'000;
constexpr std::size_t kErrorBatchSize = 32;

::grpc::Status Invalid(const std::string& message) {
  return {::grpc::StatusCode::INVALID_ARGUMENT, message};
}

class CommandTaskReactor final : public ::grpc::ServerUnaryReactor {
 public:
  using Submit = std::function<::grpc::Status(std::stop_token, CommandTicket*,
                                              CommandWaitPolicy*)>;

  CommandTaskReactor(ActiveCallbackRegistry& registry,
                     ExecuteCommandResponse* response, Submit submit)
      : response_(response),
        gate_(std::make_shared<LifetimeGate<CommandTaskReactor>>(this)) {
    const std::weak_ptr<LifetimeGate<CommandTaskReactor>> weak_gate = gate_;
    if (!register_shutdown(registry, weak_gate)) return;
    run_submit(weak_gate, std::move(submit), stop_source_.get_token());
  }

  CommandTaskReactor(BoundedExecutor& executor,
                     ActiveCallbackRegistry& registry,
                     ExecuteCommandResponse* response, Submit submit)
      : response_(response),
        gate_(std::make_shared<LifetimeGate<CommandTaskReactor>>(this)) {
    const std::weak_ptr<LifetimeGate<CommandTaskReactor>> weak_gate = gate_;
    if (!register_shutdown(registry, weak_gate)) return;
    if (!executor.submit([weak_gate, submit = std::move(submit),
                          stop_token = stop_source_.get_token()]() mutable {
          run_submit(weak_gate, std::move(submit), stop_token);
        })) {
      finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
              "blocking work queue is full"});
    }
  }

  void OnCancel() override {
    stop_source_.request_stop();
    finish({::grpc::StatusCode::CANCELLED,
            "command wait cancelled; any accepted work continues"});
  }

  void OnDone() override {
    gate_->detach();
    registration_.reset();
    delete this;
  }

  void shutdown() {
    stop_source_.request_stop();
    finish({::grpc::StatusCode::UNAVAILABLE,
            "server shutting down; any accepted command continues"});
  }

 private:
  bool register_shutdown(
      ActiveCallbackRegistry& registry,
      const std::weak_ptr<LifetimeGate<CommandTaskReactor>>& weak_gate) {
    registration_ = registry.register_callback([weak_gate] {
      if (auto gate = weak_gate.lock())
        gate->invoke([](CommandTaskReactor& reactor) { reactor.shutdown(); });
    });
    if (registration_) return true;
    shutdown();
    return false;
  }

  static void run_submit(
      const std::weak_ptr<LifetimeGate<CommandTaskReactor>>& weak_gate,
      Submit&& submit, std::stop_token stop_token) {
    CommandTicket ticket;
    CommandWaitPolicy policy = CommandWaitPolicy::Completed;
    ::grpc::Status status;
    try {
      status = submit(std::move(stop_token), &ticket, &policy);
    } catch (const std::exception& error) {
      status = {::grpc::StatusCode::INTERNAL, error.what()};
    }
    auto gate = weak_gate.lock();
    if (!gate) return;
    if (!status.ok()) {
      gate->invoke([&](CommandTaskReactor& reactor) {
        reactor.finish(std::move(status));
      });
      return;
    }
    ticket.observe(policy, [weak_gate](const CommandResult& result) {
      auto observed_gate = weak_gate.lock();
      if (!observed_gate) return;
      observed_gate->invoke(
          [&](CommandTaskReactor& reactor) { reactor.complete(result); });
    });
  }

  void complete(const CommandResult& result) {
    gate_->finish([&](CommandTaskReactor& reactor) {
      reactor.response_->set_command_sequence(result.accepted_sequence);
      if (result.state == CommandState::Failed) {
        reactor.response_->set_status(RCS_STATUS_ERROR);
        reactor.response_->mutable_error()->set_type(
            NML_MESSAGE_TYPE_NML_ERROR);
        reactor.response_->mutable_error()->set_command_sequence(
            result.sequence);
        reactor.response_->mutable_error()->set_message(result.error);
      } else if (result.state == CommandState::Accepted) {
        reactor.response_->set_status(RCS_STATUS_EXEC);
      } else {
        reactor.response_->set_status(RCS_STATUS_DONE);
      }
      reactor.Finish(::grpc::Status::OK);
    });
  }

  void finish(::grpc::Status status) {
    gate_->finish([&](CommandTaskReactor& reactor) { reactor.Finish(status); });
  }

  ExecuteCommandResponse* response_;
  std::stop_source stop_source_;
  std::shared_ptr<LifetimeGate<CommandTaskReactor>> gate_;
  ActiveCallbackRegistry::Registration registration_;
};

class MachineServiceImpl final : public MachineService::CallbackService,
                                 public ManagedGrpcService {
  struct StatusSample {
    std::uint64_t sequence = 0;
    EncodedStatus encoded;
    std::optional<LinuxCNCStatDelta> delta_from_previous;
  };
  using StatusSamplePtr = std::shared_ptr<const StatusSample>;

  struct WorkspaceActivation {
    explicit WorkspaceActivation(std::shared_ptr<ProgramWorkspaceStore> value)
        : store(std::move(value)) {}
    std::mutex mutex;
    std::string active;
    bool opening = false;
    std::shared_ptr<ProgramWorkspaceStore> store;
  };

  struct PendingWorkspaceLease {
    PendingWorkspaceLease(std::shared_ptr<WorkspaceActivation> value,
                          std::string id)
        : activation(std::move(value)), workspace(std::move(id)) {}
    ~PendingWorkspaceLease() {
      std::lock_guard lock(activation->mutex);
      activation->opening = false;
      if (release) activation->store->unpin_entry(workspace);
    }
    std::shared_ptr<WorkspaceActivation> activation;
    std::string workspace;
    bool release = true;
  };

 public:
  explicit MachineServiceImpl(const DaemonConfig& config,
                              std::shared_ptr<ProgramWorkspaceStore> workspaces,
                              std::shared_ptr<PositionTelemetry> positions,
                              BoundedExecutor& blocking,
                              AdmissionCounter& stream_admission)
      : nml_(config.nml_file, config.command_queue_capacity,
             config.command_completion_timeout),
        filesystem_(1, 16, 4),
        blocking_(blocking),
        stream_admission_(stream_admission),
        workspaces_(std::move(workspaces)),
        workspace_activation_(
            std::make_shared<WorkspaceActivation>(workspaces_)),
        positions_(std::move(positions)),
        status_period_(config.status_period),
        error_period_(config.error_period),
        position_period_(config.position_period),
        replay_capacity_(
            std::max<std::size_t>(1, config.status_replay_capacity)),
        stopping_(false),
        position_poller_([this] { poll_positions(); }) {}

  ~MachineServiceImpl() override { shutdown(); }

  ::grpc::Service* service() noexcept override { return this; }

  void shutdown() override {
    if (stopping_.exchange(true, std::memory_order_relaxed)) return;
    status_wakes_.close();
    error_wakes_.close();
    callbacks_.shutdown();
    filesystem_.shutdown();
    position_condition_.notify_all();
    if (position_poller_.joinable()) position_poller_.join();
  }

  ::grpc::ServerUnaryReactor* GetStatus(::grpc::CallbackServerContext* context,
                                        const GetStatusRequest*,
                                        GetStatusResponse* response) override {
    auto* reactor = context->DefaultReactor();
    const auto sample = read_status();
    if (!sample) {
      reactor->Finish({::grpc::StatusCode::UNAVAILABLE,
                       "LinuxCNC NML status channel is unavailable"});
      return reactor;
    }
    response->set_sequence(sample->sequence);
    *response->mutable_status() = sample->encoded.message;
    reactor->Finish(::grpc::Status::OK);
    return reactor;
  }

  ::grpc::ServerWriteReactor<WatchStatusEvent>* WatchStatus(
      ::grpc::CallbackServerContext*,
      const WatchStatusRequest* request) override {
    return new StatusReactor(*this, request->after_sequence());
  }

  ::grpc::ServerUnaryReactor* ExecuteCommand(
      ::grpc::CallbackServerContext*, const ExecuteCommandRequest* request,
      ExecuteCommandResponse* response) override {
    auto owned_request = std::make_shared<ExecuteCommandRequest>(*request);
    auto submit = [this, owned_request = std::move(owned_request)](
                      std::stop_token stop_token, CommandTicket* ticket,
                      CommandWaitPolicy* policy) {
      return submit_command(std::move(stop_token), owned_request.get(), ticket,
                            policy);
    };
    if (request->command_case() == ExecuteCommandRequest::kProgramOpen) {
      return new CommandTaskReactor(filesystem_, callbacks_, response,
                                    std::move(submit));
    }
    return new CommandTaskReactor(callbacks_, response, std::move(submit));
  }

  ::grpc::Status submit_command(std::stop_token stop_token,
                                const ExecuteCommandRequest* request,
                                CommandTicket* ticket,
                                CommandWaitPolicy* policy) {
    NmlCommand command;
    switch (request->command_case()) {
      case ExecuteCommandRequest::kSetTaskMode:
        command.kind = NmlCommandKind::SetTaskMode;
        command.integer =
            static_cast<std::int32_t>(request->set_task_mode().mode());
        break;
      case ExecuteCommandRequest::kSetState:
        command.kind = NmlCommandKind::SetTaskState;
        command.integer =
            static_cast<std::int32_t>(request->set_state().state());
        break;
      case ExecuteCommandRequest::kTaskPlanSynch:
        command.kind = NmlCommandKind::TaskPlanSynch;
        break;
      case ExecuteCommandRequest::kResetInterpreter:
        command.kind = NmlCommandKind::ResetInterpreter;
        break;
      case ExecuteCommandRequest::kProgramOpen:
        command.kind = NmlCommandKind::ProgramOpen;
        if (!workspaces_ ||
            request->program_open().entry().workspace_id().empty())
          return Invalid("program_open requires a workspace handle");
        {
          const auto workspace = request->program_open().entry().workspace_id();
          const auto relative_path =
              request->program_open().entry().relative_path();
          {
            std::lock_guard lock(workspace_activation_->mutex);
            if (workspace_activation_->opening) {
              return ::grpc::Status(::grpc::StatusCode::RESOURCE_EXHAUSTED,
                                    "a program open is already pending");
            }
            workspace_activation_->opening = true;
          }
          std::filesystem::path resolved_entry;
          if (!workspaces_->pin_entry(workspace, relative_path,
                                      &resolved_entry)) {
            std::lock_guard lock(workspace_activation_->mutex);
            workspace_activation_->opening = false;
            return Invalid("program workspace entry is missing or unsafe");
          }
          auto lease = std::make_shared<PendingWorkspaceLease>(
              workspace_activation_, workspace);
          const auto final_entry =
              workspaces_->active_directory() /
              std::filesystem::path(relative_path).lexically_normal();
          if (final_entry.string().size() >= kNmlProgramPathCapacity) {
            return Invalid("program path exceeds LinuxCNC's path limit");
          }
          auto revision = workspaces_->stage(
              workspace, relative_path, [executor = &filesystem_](auto path) {
                auto cleanup = [path = std::move(path)] {
                  std::error_code error;
                  std::filesystem::remove_all(path, error);
                };
                try {
                  (void)executor->submit_cleanup(std::move(cleanup));
                } catch (...) {
                  // Startup reconciliation removes an orphan if cleanup
                  // admission itself cannot allocate.
                  return;
                }
              });
          if (!revision) {
            return Invalid(
                "program workspace entry became unavailable or unsafe");
          }
          command.path = final_entry.string();
          command.prepare = [revision, lease](NmlCommand&) {
            if (!revision->commit())
              throw std::runtime_error("failed to activate staged program");
          };
          command.on_completed = [activation = workspace_activation_, workspace,
                                  revision, lease] {
            revision->complete();
            activation->store->release_recovered_leases();
            std::lock_guard lock(activation->mutex);
            activation->opening = false;
            if (activation->active == workspace) {
              lease->release = true;
              return;
            }
            lease->release = false;  // temporary lease becomes the active lease
            if (!activation->active.empty())
              activation->store->unpin_entry(activation->active);
            activation->active = workspace;
          };
          command.on_failed = [revision] { revision->rollback(); };
        }
        break;
      case ExecuteCommandRequest::kProgramClose:
        command.kind = NmlCommandKind::ProgramClose;
        command.on_completed = [activation = workspace_activation_] {
          activation->store->release_recovered_leases();
          {
            std::lock_guard lock(activation->mutex);
            if (!activation->active.empty()) {
              activation->store->unpin_entry(activation->active);
              activation->active.clear();
            }
          }
          activation->store->clear_active_link();
        };
        break;
      case ExecuteCommandRequest::kRunProgram:
        command.kind = NmlCommandKind::Run;
        command.integer = request->run_program().start_line();
        break;
      case ExecuteCommandRequest::kPauseProgram:
        command.kind = NmlCommandKind::Pause;
        break;
      case ExecuteCommandRequest::kResumeProgram:
        command.kind = NmlCommandKind::Resume;
        break;
      case ExecuteCommandRequest::kStepProgram:
        command.kind = NmlCommandKind::Step;
        break;
      case ExecuteCommandRequest::kReverseProgram:
        command.kind = NmlCommandKind::Reverse;
        break;
      case ExecuteCommandRequest::kForwardProgram:
        command.kind = NmlCommandKind::Forward;
        break;
      case ExecuteCommandRequest::kStopProgram:
        command.kind = NmlCommandKind::StopProgram;
        break;
      case ExecuteCommandRequest::kAbortTask:
        command.kind = NmlCommandKind::AbortTask;
        break;
      case ExecuteCommandRequest::kSetOptionalStop:
        command.kind = NmlCommandKind::SetOptionalStop;
        command.boolean = request->set_optional_stop().enable();
        break;
      case ExecuteCommandRequest::kSetBlockDelete:
        command.kind = NmlCommandKind::SetBlockDelete;
        command.boolean = request->set_block_delete().enable();
        break;
      case ExecuteCommandRequest::kMdi:
        command.kind = NmlCommandKind::Mdi;
        command.text = request->mdi().command();
        break;
      case ExecuteCommandRequest::kSetTrajMode:
        command.kind = NmlCommandKind::SetTrajMode;
        command.integer =
            static_cast<std::int32_t>(request->set_traj_mode().mode());
        break;
      case ExecuteCommandRequest::kSetMaxVelocity:
        command.kind = NmlCommandKind::SetMaxVelocity;
        command.number = request->set_max_velocity().velocity();
        break;
      case ExecuteCommandRequest::kSetFeedRate:
        command.kind = NmlCommandKind::SetFeedRate;
        command.number = request->set_feed_rate().scale();
        break;
      case ExecuteCommandRequest::kSetRapidRate:
        command.kind = NmlCommandKind::SetRapidRate;
        command.number = request->set_rapid_rate().scale();
        break;
      case ExecuteCommandRequest::kSetSpindleOverride:
        command.kind = NmlCommandKind::SetSpindleOverride;
        command.number = request->set_spindle_override().scale();
        command.integer = request->set_spindle_override().spindle_index();
        break;
      case ExecuteCommandRequest::kOverrideLimits:
        command.kind = NmlCommandKind::OverrideLimits;
        break;
      case ExecuteCommandRequest::kTeleopEnable:
        command.kind = NmlCommandKind::TeleopEnable;
        command.boolean = request->teleop_enable().enable();
        break;
      case ExecuteCommandRequest::kSetFeedOverrideEnable:
        command.kind = NmlCommandKind::SetFeedOverrideEnable;
        command.boolean = request->set_feed_override_enable().enable();
        break;
      case ExecuteCommandRequest::kSetSpindleOverrideEnable:
        command.kind = NmlCommandKind::SetSpindleOverrideEnable;
        command.boolean = request->set_spindle_override_enable().enable();
        break;
      case ExecuteCommandRequest::kSetFeedHoldEnable:
        command.kind = NmlCommandKind::SetFeedHoldEnable;
        command.boolean = request->set_feed_hold_enable().enable();
        break;
      case ExecuteCommandRequest::kSetAdaptiveFeedEnable:
        command.kind = NmlCommandKind::SetAdaptiveFeedEnable;
        command.boolean = request->set_adaptive_feed_enable().enable();
        break;
      case ExecuteCommandRequest::kSetJointAvailability:
        command.kind = NmlCommandKind::SetJointAvailability;
        command.integer = request->set_joint_availability().joint_index();
        command.boolean = request->set_joint_availability().available();
        break;
      case ExecuteCommandRequest::kSetSpindleAvailability:
        command.kind = NmlCommandKind::SetSpindleAvailability;
        command.integer = request->set_spindle_availability().spindle_index();
        command.boolean = request->set_spindle_availability().available();
        break;
      case ExecuteCommandRequest::kHomeJoint:
        command.kind = NmlCommandKind::HomeJoint;
        command.integer = request->home_joint().joint_index();
        break;
      case ExecuteCommandRequest::kUnhomeJoint:
        command.kind = NmlCommandKind::UnhomeJoint;
        command.integer = request->unhome_joint().joint_index();
        break;
      case ExecuteCommandRequest::kJogStop:
        command.kind = NmlCommandKind::JogStop;
        command.integer = request->jog_stop().axis_or_joint_index();
        command.boolean = request->jog_stop().is_joint_jog();
        break;
      case ExecuteCommandRequest::kJogContinuous:
        command.kind = NmlCommandKind::JogContinuous;
        command.integer = request->jog_continuous().axis_or_joint_index();
        command.boolean = request->jog_continuous().is_joint_jog();
        command.number = request->jog_continuous().speed();
        break;
      case ExecuteCommandRequest::kJogIncrement:
        command.kind = NmlCommandKind::JogIncrement;
        command.integer = request->jog_increment().axis_or_joint_index();
        command.boolean = request->jog_increment().is_joint_jog();
        command.number = request->jog_increment().speed();
        command.number2 = request->jog_increment().increment();
        break;
      case ExecuteCommandRequest::kSetMinPositionLimit:
        command.kind = NmlCommandKind::SetMinPositionLimit;
        command.integer = request->set_min_position_limit().joint_index();
        command.number = request->set_min_position_limit().limit();
        break;
      case ExecuteCommandRequest::kSetMaxPositionLimit:
        command.kind = NmlCommandKind::SetMaxPositionLimit;
        command.integer = request->set_max_position_limit().joint_index();
        command.number = request->set_max_position_limit().limit();
        break;
      case ExecuteCommandRequest::kSpindleOn:
        command.kind = NmlCommandKind::SpindleOn;
        command.number = request->spindle_on().speed();
        command.integer = request->spindle_on().spindle_index();
        command.boolean = !request->spindle_on().has_wait_for_speed() ||
                          request->spindle_on().wait_for_speed();
        break;
      case ExecuteCommandRequest::kSpindleIncrease:
        command.kind = NmlCommandKind::SpindleIncrease;
        command.integer = request->spindle_increase().spindle_index();
        break;
      case ExecuteCommandRequest::kSpindleDecrease:
        command.kind = NmlCommandKind::SpindleDecrease;
        command.integer = request->spindle_decrease().spindle_index();
        break;
      case ExecuteCommandRequest::kSpindleOff:
        command.kind = NmlCommandKind::SpindleOff;
        command.integer = request->spindle_off().spindle_index();
        break;
      case ExecuteCommandRequest::kSpindleBrake:
        command.kind = NmlCommandKind::SpindleBrake;
        command.integer = request->spindle_brake().spindle_index();
        command.boolean = request->spindle_brake().engage();
        break;
      case ExecuteCommandRequest::kSetMist:
        command.kind = NmlCommandKind::SetMist;
        command.boolean = request->set_mist().on();
        break;
      case ExecuteCommandRequest::kSetFlood:
        command.kind = NmlCommandKind::SetFlood;
        command.boolean = request->set_flood().on();
        break;
      case ExecuteCommandRequest::kLoadToolTable:
        command.kind = NmlCommandKind::LoadToolTable;
        break;
      case ExecuteCommandRequest::kSetTool: {
        if (!request->set_tool().has_tool())
          return Invalid("set_tool.tool is required");
        command.kind = NmlCommandKind::SetTool;
        const auto& source = request->set_tool().tool();
        if (source.offset().values_size() > 9 ||
            source.wear_offset().values_size() > 9)
          return Invalid("tool offsets must contain at most 9 axes");
        command.tool.tool_no = source.tool_no();
        command.tool.has_pocket_no = source.has_pocket_no();
        command.tool.pocket_no = source.pocket_no();
        command.tool.has_offset = source.has_offset();
        command.tool.has_wear_offset = source.has_wear_offset();
        command.tool.has_diameter = source.has_diameter();
        command.tool.diameter = source.diameter();
        command.tool.has_front_angle = source.has_front_angle();
        command.tool.front_angle = source.front_angle();
        command.tool.has_back_angle = source.has_back_angle();
        command.tool.back_angle = source.back_angle();
        command.tool.has_orientation = source.has_orientation();
        command.tool.orientation = source.orientation();
        command.tool.has_comment = source.has_comment();
        command.tool.comment = source.comment();
        command.tool.offset_values =
            std::min<int>(source.offset().values_size(), 9);
        for (int index = 0; index < source.offset().values_size() && index < 9;
             ++index)
          command.tool.offset.values[static_cast<std::size_t>(index)] =
              source.offset().values(index);
        command.tool.wear_offset_values =
            std::min<int>(source.wear_offset().values_size(), 9);
        for (int index = 0;
             index < source.wear_offset().values_size() && index < 9; ++index)
          command.tool.wear_offset.values[static_cast<std::size_t>(index)] =
              source.wear_offset().values(index);
        break;
      }
      case ExecuteCommandRequest::kDeleteTool:
        command.kind = NmlCommandKind::DeleteTool;
        command.integer = request->delete_tool().tool_no();
        break;
      case ExecuteCommandRequest::kSetDigitalOutput:
        if (request->set_digital_output().index() < 0 ||
            request->set_digital_output().index() >=
                NmlAdapter::kDigitalOutputLimit) {
          return Invalid("digital output index is out of range");
        }
        command.kind = NmlCommandKind::SetDigitalOutput;
        command.integer = request->set_digital_output().index();
        command.boolean = request->set_digital_output().value();
        break;
      case ExecuteCommandRequest::kSetAnalogOutput:
        if (request->set_analog_output().index() < 0 ||
            request->set_analog_output().index() >=
                NmlAdapter::kAnalogOutputLimit) {
          return Invalid("analog output index is out of range");
        }
        command.kind = NmlCommandKind::SetAnalogOutput;
        command.integer = request->set_analog_output().index();
        command.number = request->set_analog_output().value();
        break;
      case ExecuteCommandRequest::kSetDebugLevel:
        command.kind = NmlCommandKind::SetDebugLevel;
        command.integer = request->set_debug_level().level();
        break;
      case ExecuteCommandRequest::kSendOperatorError:
        command.kind = NmlCommandKind::SendOperatorError;
        command.text = request->send_operator_error().message();
        break;
      case ExecuteCommandRequest::kSendOperatorText:
        command.kind = NmlCommandKind::SendOperatorText;
        command.text = request->send_operator_text().message();
        break;
      case ExecuteCommandRequest::kSendOperatorDisplay:
        command.kind = NmlCommandKind::SendOperatorDisplay;
        command.text = request->send_operator_display().message();
        break;
      default:
        return Invalid("execute_command requires a supported command oneof");
    }
    const auto configuration = read_native_status();
    const auto* configuration_ptr = configuration.get();
    const auto validation = validate_nml_command(command, configuration_ptr);
    if (validation.code == NmlCommandValidationCode::StatusUnavailable) {
      return {::grpc::StatusCode::UNAVAILABLE, validation.message};
    }
    if (!validation) return Invalid(validation.message);
    auto submission = nml_.submit(std::move(command), std::move(stop_token));
    if (submission.status == CommandSubmitStatus::QueueFull) {
      return {::grpc::StatusCode::RESOURCE_EXHAUSTED, "command queue is full"};
    }
    if (submission.status == CommandSubmitStatus::Stopped) {
      return {::grpc::StatusCode::UNAVAILABLE,
              "command coordinator is stopped"};
    }
    *ticket = std::move(submission.ticket);
    *policy = request->wait_policy() == WAIT_POLICY_ACCEPTED
                  ? CommandWaitPolicy::Accepted
                  : CommandWaitPolicy::Completed;
    return ::grpc::Status::OK;
  }

  ::grpc::ServerWriteReactor<LinuxCNCError>* WatchErrors(
      ::grpc::CallbackServerContext*,
      const WatchErrorsRequest* request) override {
    return new ErrorReactor(*this, request->after_sequence());
  }

  ::grpc::ServerUnaryReactor* ConfigurePositionHistory(
      ::grpc::CallbackServerContext*, const PositionHistoryConfig* request,
      google::protobuf::Empty* response) override {
    auto owned_request = std::make_shared<PositionHistoryConfig>(*request);
    return new detail::UnaryTaskReactor<google::protobuf::Empty>(
        blocking_, callbacks_, response,
        [this, owned_request = std::move(owned_request)](
            const std::stop_token& stop_token, google::protobuf::Empty*) {
          if (stop_token.stop_requested()) {
            return ::grpc::Status(::grpc::StatusCode::CANCELLED,
                                  "RPC cancelled");
          }
          return configure_position(*owned_request);
        });
  }

  ::grpc::Status configure_position(const PositionHistoryConfig& request) {
    if (request.capacity() > 100000) {
      return {::grpc::StatusCode::RESOURCE_EXHAUSTED,
              "position history capacity exceeds 100000 samples"};
    }
    if (request.sample_period_ms() > kMaxPositionSamplePeriodMs) {
      return Invalid("position history sample period exceeds 60000 ms");
    }
    {
      std::lock_guard lock(position_mutex_);
      if (request.has_enabled()) position_enabled_ = request.enabled();
      if (request.sample_period_ms() > 0) {
        position_period_ =
            std::chrono::milliseconds(request.sample_period_ms());
      }
      ++position_config_generation_;
      if (request.capacity() > 0) positions_->configure(request.capacity());
      if (request.has_enabled() && !request.enabled()) {
        position_reader_available_ = false;
        positions_->clear();
      }
    }
    position_condition_.notify_all();
    return ::grpc::Status::OK;
  }

  ::grpc::ServerUnaryReactor* ClearPositionHistory(
      ::grpc::CallbackServerContext*, const google::protobuf::Empty*,
      google::protobuf::Empty* response) override {
    return new detail::UnaryTaskReactor<google::protobuf::Empty>(
        blocking_, callbacks_, response,
        [this](const std::stop_token& stop_token, google::protobuf::Empty*) {
          if (stop_token.stop_requested()) {
            return ::grpc::Status(::grpc::StatusCode::CANCELLED,
                                  "RPC cancelled");
          }
          positions_->clear();
          return ::grpc::Status::OK;
        });
  }

 private:
  class StatusReactor final
      : public ::grpc::ServerWriteReactor<WatchStatusEvent> {
   public:
    StatusReactor(MachineServiceImpl& service, std::uint64_t after)
        : service_(service),
          after_(after),
          admitted_(service_.stream_admission_.acquire()),
          gate_(std::make_shared<LifetimeGate<StatusReactor>>(this)) {
      const std::weak_ptr<LifetimeGate<StatusReactor>> weak_gate = gate_;
      registration_ = service_.callbacks_.register_callback([weak_gate] {
        if (auto gate = weak_gate.lock())
          gate->invoke([](StatusReactor& reactor) { reactor.shutdown(); });
      });
      if (!registration_) {
        shutdown();
        return;
      }
      if (!admitted_) {
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "stream admission limit reached"});
        return;
      }
      subscription_ =
          service_.status_wakes_.subscribe([weak_gate](const std::uint64_t&) {
            auto gate = weak_gate.lock();
            if (gate)
              gate->invoke(
                  [](StatusReactor& reactor) { reactor.schedule_wake(); });
          });
      schedule_wake();
    }

    void OnWriteDone(bool ok) override {
      gate_->invoke([ok](StatusReactor& reactor) { reactor.write_done(ok); });
    }

    void OnCancel() override {
      gate_->invoke([](StatusReactor& reactor) {
        reactor.subscription_.reset();
        reactor.request_finish(
            {::grpc::StatusCode::CANCELLED, "status stream cancelled"});
      });
    }

    void OnDone() override {
      subscription_.reset();
      gate_->detach();
      registration_.reset();
      if (admitted_) service_.stream_admission_.release();
      delete this;
    }

    void shutdown() {
      subscription_.reset();
      request_finish({::grpc::StatusCode::UNAVAILABLE, "server shutting down"});
    }

   private:
    void write_done(bool ok) {
      write_finish_.complete_write(ok);
      if (finish_if_ready() || !ok) return;
      previous_ = std::move(writing_sample_);
      cursor_ = previous_->sequence;
      initial_written_ = true;
      schedule_wake();
    }
    void schedule_wake() {
      if (write_finish_.termination_requested() ||
          gate_->state() != LifetimeGate<StatusReactor>::State::Open ||
          wake_scheduled_.exchange(true))
        return;
      const std::weak_ptr<LifetimeGate<StatusReactor>> weak_gate = gate_;
      if (!service_.blocking_.submit([weak_gate] {
            auto gate = weak_gate.lock();
            if (gate)
              gate->invoke([](StatusReactor& reactor) {
                reactor.wake_scheduled_.store(false);
                reactor.wake();
              });
          })) {
        wake_scheduled_.store(false);
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "wire encoding queue is full"});
      }
    }

    void wake() {
      if (write_finish_.write_in_flight() ||
          write_finish_.termination_requested() ||
          gate_->state() != LifetimeGate<StatusReactor>::State::Open)
        return;
      if (!service_.status_available()) {
        request_finish({::grpc::StatusCode::UNAVAILABLE,
                        "LinuxCNC NML status snapshot is stale"});
        return;
      }
      auto selection = service_.select_status_history(
          initial_written_ ? cursor_ : after_, !initial_written_);
      if (selection.entries.empty()) {
        request_finish({::grpc::StatusCode::UNAVAILABLE,
                        "LinuxCNC NML status snapshot is unavailable"});
        return;
      }
      auto latest = selection.entries.back();
      message_.Clear();
      if (!initial_written_) {
        if (after_ != 0 && selection.anchor_retained) {
          message_.set_sequence(latest->sequence);
          auto* replay = message_.mutable_replay();
          replay->set_from_sequence(after_);
          replay->set_to_sequence(latest->sequence);
          *replay->mutable_snapshot() =
              selection.entries.front()->encoded.message;
          for (auto entry = std::next(selection.entries.begin());
               entry != selection.entries.end(); ++entry) {
            if ((*entry)->delta_from_previous)
              *replay->add_deltas() = *(*entry)->delta_from_previous;
          }
        } else {
          message_.set_sequence(latest->sequence);
          *message_.mutable_snapshot() = latest->encoded.message;
        }
      } else {
        if (latest->sequence == cursor_) return;
        if (!selection.anchor_retained) {
          message_.set_sequence(latest->sequence);
          *message_.mutable_snapshot() = latest->encoded.message;
        } else {
          auto delta =
              previous_->sequence + 1 == latest->sequence
                  ? latest->delta_from_previous
                  : make_status_delta(previous_->encoded, latest->encoded,
                                      latest->sequence);
          if (!delta) return;
          message_.set_sequence(latest->sequence);
          *message_.mutable_delta() = std::move(*delta);
        }
      }
      writing_sample_ = std::move(latest);
      if (!write_finish_.try_start_write()) return;
      StartWrite(&message_);
    }

    void request_finish(::grpc::Status status) {
      subscription_.reset();
      write_finish_.request_finish(std::move(status));
      finish_if_ready();
    }

    bool finish_if_ready() {
      auto status = write_finish_.take_finish_status();
      if (!status) return false;
      gate_->finish([status = std::move(*status)](StatusReactor& reactor) {
        reactor.subscription_.reset();
        reactor.Finish(status);
      });
      return true;
    }

    MachineServiceImpl& service_;
    const std::uint64_t after_;
    bool admitted_ = false;
    bool initial_written_ = false;
    std::uint64_t cursor_ = 0;
    StatusSamplePtr previous_;
    StatusSamplePtr writing_sample_;
    WatchStatusEvent message_;
    std::atomic<bool> wake_scheduled_{false};
    DeferredWriteFinish write_finish_;
    SubscriptionHub<std::uint64_t>::Subscription subscription_;
    std::shared_ptr<LifetimeGate<StatusReactor>> gate_;
    ActiveCallbackRegistry::Registration registration_;
  };

  class ErrorReactor final : public ::grpc::ServerWriteReactor<LinuxCNCError> {
   public:
    ErrorReactor(MachineServiceImpl& service, std::uint64_t after_sequence)
        : service_(service),
          admitted_(service_.stream_admission_.acquire()),
          gate_(std::make_shared<LifetimeGate<ErrorReactor>>(this)) {
      const std::weak_ptr<LifetimeGate<ErrorReactor>> weak_gate = gate_;
      registration_ = service_.callbacks_.register_callback([weak_gate] {
        if (auto gate = weak_gate.lock())
          gate->invoke([](ErrorReactor& reactor) { reactor.shutdown(); });
      });
      if (!registration_) {
        shutdown();
        return;
      }
      if (!admitted_) {
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "stream admission limit reached"});
        return;
      }
      cursor_ = after_sequence;
      subscription_ =
          service_.error_wakes_.subscribe([weak_gate](const std::uint64_t&) {
            auto gate = weak_gate.lock();
            if (gate)
              gate->invoke([](ErrorReactor& reactor) { reactor.wake(); });
          });
      gate_->invoke([](ErrorReactor& reactor) { reactor.wake(); });
    }

    void OnWriteDone(bool ok) override {
      gate_->invoke([ok](ErrorReactor& reactor) { reactor.write_done(ok); });
    }

    void OnCancel() override {
      gate_->invoke([](ErrorReactor& reactor) {
        reactor.subscription_.reset();
        reactor.request_finish(
            {::grpc::StatusCode::CANCELLED, "error stream cancelled"});
      });
    }

    void OnDone() override {
      subscription_.reset();
      gate_->detach();
      registration_.reset();
      if (admitted_) service_.stream_admission_.release();
      delete this;
    }

    void shutdown() {
      subscription_.reset();
      request_finish({::grpc::StatusCode::UNAVAILABLE, "server shutting down"});
    }

   private:
    void write_done(bool ok) {
      write_finish_.complete_write(ok);
      if (finish_if_ready() || !ok) return;
      cursor_ = writing_sequence_;
      wake();
    }
    void wake() {
      if (write_finish_.write_in_flight() ||
          write_finish_.termination_requested() ||
          gate_->state() != LifetimeGate<ErrorReactor>::State::Open)
        return;
      const auto available = service_.errors_.after(cursor_);
      if (available.behind) {
        request_finish({::grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "error reader fell behind retained events"});
        return;
      }
      if (available.entries.empty()) return;
      const auto& entry = available.entries.front();
      message_.Clear();
      message_.set_type(static_cast<NmlMessageType>(entry.event.type));
      message_.set_message(entry.event.message);
      message_.set_sequence(entry.sequence);
      writing_sequence_ = entry.sequence;
      if (!write_finish_.try_start_write()) return;
      StartWrite(&message_);
    }

    void request_finish(::grpc::Status status) {
      subscription_.reset();
      write_finish_.request_finish(std::move(status));
      finish_if_ready();
    }

    bool finish_if_ready() {
      auto status = write_finish_.take_finish_status();
      if (!status) return false;
      gate_->finish([status = std::move(*status)](ErrorReactor& reactor) {
        reactor.subscription_.reset();
        reactor.Finish(status);
      });
      return true;
    }

    MachineServiceImpl& service_;
    bool admitted_ = false;
    std::uint64_t cursor_ = 0;
    std::uint64_t writing_sequence_ = 0;
    LinuxCNCError message_;
    DeferredWriteFinish write_finish_;
    SubscriptionHub<std::uint64_t>::Subscription subscription_;
    std::shared_ptr<LifetimeGate<ErrorReactor>> gate_;
    ActiveCallbackRegistry::Registration registration_;
  };

  struct StatusHistorySelection {
    bool anchor_retained = false;
    std::vector<StatusSamplePtr> entries;
  };

  StatusSamplePtr read_status() const {
    std::lock_guard lock(status_mutex_);
    return status_available_ ? latest_ : nullptr;
  }

  std::shared_ptr<const NmlStatusSnapshot> read_native_status() const {
    std::lock_guard lock(status_mutex_);
    return status_available_ ? latest_native_ : nullptr;
  }

  bool status_available() const {
    std::lock_guard lock(status_mutex_);
    return status_available_;
  }

  void mark_status_unavailable() {
    bool notify = false;
    std::uint64_t sequence = 0;
    {
      std::lock_guard lock(status_mutex_);
      notify = status_available_;
      status_available_ = false;
      sequence = sequence_;
    }
    if (notify) status_wakes_.publish(sequence);
  }

  StatusHistorySelection select_status_history(std::uint64_t anchor,
                                               bool include_replay) const {
    std::lock_guard lock(status_mutex_);
    StatusHistorySelection selection;
    if (history_.empty()) return selection;
    const auto found = std::find_if(
        history_.begin(), history_.end(),
        [anchor](const auto& entry) { return entry->sequence == anchor; });
    selection.anchor_retained = found != history_.end();
    if (include_replay && anchor != 0 && selection.anchor_retained) {
      selection.entries.assign(found, history_.end());
    } else {
      selection.entries.push_back(history_.back());
    }
    return selection;
  }

  void poll_positions() {
    auto next_status = std::chrono::steady_clock::now();
    auto next_error = next_status;
    auto next_position = next_status;
    while (!stopping_.load(std::memory_order_relaxed)) {
      bool enabled = false;
      std::chrono::milliseconds position_period;
      std::uint64_t generation = 0;
      {
        std::lock_guard lock(position_mutex_);
        enabled = position_enabled_;
        position_period = position_period_;
        generation = position_config_generation_;
      }
      const auto now = std::chrono::steady_clock::now();
      const bool status_due = now >= next_status;
      const bool position_due = enabled && now >= next_position;
      if (status_due || position_due) {
        NmlStatusSnapshot snapshot;
        NmlPositionSnapshot position_snapshot;
        const auto status_poll = status_due
                                     ? nml_.poll_status(&snapshot)
                                     : nml_.poll_position(&position_snapshot);
        if (status_poll == NmlStatusPoll::Updated) {
          bool recovered = false;
          if (status_due) {
            {
              std::lock_guard lock(status_mutex_);
              recovered = !status_available_;
              status_available_ = true;
            }
            if (recovered) positions_->clear();
          }
          if (position_due) {
            PositionSample sample;
            for (std::size_t index = 0; index < sample.coordinates.size();
                 ++index) {
              sample.coordinates[index] =
                  status_due
                      ? snapshot.motion_stat.traj.position.values[index] -
                            snapshot.task_stat.tool_offset.values[index]
                      : position_snapshot.commanded[index] -
                            position_snapshot.tool_offset[index];
            }
            sample.motion_type = status_due ? snapshot.motion_type
                                            : position_snapshot.motion_type;
            // Configuration changes linearize with the append. In particular,
            // a poll that started before disable cannot repopulate the history
            // after disable cleared it.
            std::lock_guard lock(position_mutex_);
            if (position_enabled_ &&
                position_config_generation_ == generation) {
              position_reader_available_ = true;
              positions_->append(sample);
            }
          }
          if (status_due) observe_status(std::move(snapshot), recovered);
        } else if (status_poll == NmlStatusPoll::Stale ||
                   status_poll == NmlStatusPoll::Disconnected) {
          if (status_due) {
            bool was_available = false;
            {
              std::lock_guard lock(status_mutex_);
              was_available = status_available_;
            }
            if (was_available) positions_->clear();
            mark_status_unavailable();
          } else {
            // Position telemetry has its own NML reader. Its failure must not
            // invalidate the authoritative full-status stream.
            std::lock_guard lock(position_mutex_);
            if (position_reader_available_) {
              position_reader_available_ = false;
              positions_->clear();
            }
          }
        }
        if (status_due) {
          do {
            next_status += status_period_;
          } while (next_status <= now);
        }
        if (position_due) {
          do {
            next_position += position_period;
          } while (next_position <= now);
        }
      } else if (!enabled) {
        next_position = now + position_period;
      }
      if (now >= next_error) {
        std::size_t drained = 0;
        while (drained < kErrorBatchSize) {
          auto error = nml_.poll_error();
          if (!error) break;
          const auto sequence = errors_.publish(std::move(*error));
          error_wakes_.publish(sequence);
          ++drained;
        }
        if (drained == kErrorBatchSize) {
          next_error = now;
        } else {
          do {
            next_error += error_period_;
          } while (next_error <= now);
        }
      }
      std::unique_lock lock(position_mutex_);
      auto deadline = std::min(next_status, next_error);
      if (position_enabled_) deadline = std::min(deadline, next_position);
      position_condition_.wait_until(lock, deadline, [this, generation] {
        return stopping_.load(std::memory_order_relaxed) ||
               position_config_generation_ != generation;
      });
      if (position_config_generation_ != generation) {
        next_position = std::chrono::steady_clock::now();
      }
    }
  }

  void observe_status(NmlStatusSnapshot fresh, bool force_rebase = false) {
    // ProgramClose may originate outside this RPC service. Reconcile the pin
    // from authoritative LinuxCNC status so TTL cleanup resumes once no file
    // is open, regardless of which client initiated the close.
    if (fresh.task_stat.file.empty() && fresh.file.empty()) {
      std::lock_guard activation_lock(workspace_activation_->mutex);
      if (!workspace_activation_->opening) {
        bool clear_active =
            workspace_activation_->store->release_recovered_leases();
        if (!workspace_activation_->active.empty()) {
          workspace_activation_->store->unpin_entry(
              workspace_activation_->active);
          workspace_activation_->active.clear();
          clear_active = true;
        }
        if (clear_active) workspace_activation_->store->clear_active_link();
      }
    }
    auto encoded = encode_status(fresh);
    std::uint64_t published = 0;
    std::unique_lock lock(status_mutex_);
    if (!latest_) {
      sequence_ = 1;
      latest_native_ =
          std::make_shared<const NmlStatusSnapshot>(std::move(fresh));
      latest_ = std::make_shared<const StatusSample>(
          StatusSample{sequence_, std::move(encoded), std::nullopt});
      history_.push_back(latest_);
      published = sequence_;
    } else if (force_rebase) {
      ++sequence_;
      latest_native_ =
          std::make_shared<const NmlStatusSnapshot>(std::move(fresh));
      latest_ = std::make_shared<const StatusSample>(
          StatusSample{sequence_, std::move(encoded), std::nullopt});
      history_.clear();
      history_.push_back(latest_);
      published = sequence_;
    } else {
      auto delta = make_status_delta(latest_->encoded, encoded, sequence_ + 1);
      if (!delta) {
        lock.unlock();
        return;
      }
      ++sequence_;
      latest_native_ =
          std::make_shared<const NmlStatusSnapshot>(std::move(fresh));
      latest_ = std::make_shared<const StatusSample>(
          StatusSample{sequence_, std::move(encoded), std::move(delta)});
      history_.push_back(latest_);
      while (history_.size() > replay_capacity_) history_.pop_front();
      published = sequence_;
    }
    lock.unlock();
    if (published != 0) status_wakes_.publish(published);
  }

  NmlAdapter nml_;
  BoundedExecutor filesystem_;
  BoundedExecutor& blocking_;
  AdmissionCounter& stream_admission_;
  std::shared_ptr<ProgramWorkspaceStore> workspaces_;
  std::shared_ptr<WorkspaceActivation> workspace_activation_;
  std::shared_ptr<PositionTelemetry> positions_;
  const std::chrono::milliseconds status_period_;
  const std::chrono::milliseconds error_period_;
  std::chrono::milliseconds position_period_;
  const std::size_t replay_capacity_;
  mutable std::mutex status_mutex_;
  std::shared_ptr<const NmlStatusSnapshot> latest_native_;
  StatusSamplePtr latest_;
  bool status_available_ = false;
  std::uint64_t sequence_ = 0;
  std::deque<StatusSamplePtr> history_;
  SequencedRing<NmlErrorEvent> errors_{256};
  SubscriptionHub<std::uint64_t> error_wakes_;
  SubscriptionHub<std::uint64_t> status_wakes_;
  ActiveCallbackRegistry callbacks_;
  std::mutex position_mutex_;
  std::condition_variable position_condition_;
  bool position_enabled_ = true;
  bool position_reader_available_ = false;
  std::uint64_t position_config_generation_ = 0;
  std::atomic<bool> stopping_;
  std::thread position_poller_;
};
}  // namespace

std::unique_ptr<ManagedGrpcService> make_machine_service(
    const DaemonConfig& config,
    std::shared_ptr<ProgramWorkspaceStore> workspaces,
    std::shared_ptr<PositionTelemetry> positions, BoundedExecutor& blocking,
    AdmissionCounter& stream_admission) {
  return std::make_unique<MachineServiceImpl>(config, std::move(workspaces),
                                              std::move(positions), blocking,
                                              stream_admission);
}

}  // namespace linuxcnc::server::detail
