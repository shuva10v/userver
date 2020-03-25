#include <components/manager.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <components/component_list.hpp>
#include <engine/async.hpp>
#include <logging/log.hpp>
#include <utils/async.hpp>

#include <engine/task/task_processor.hpp>
#include <engine/task/task_processor_pools.hpp>
#include "manager_config.hpp"

namespace {

const std::string kEngineMonitorDataName = "engine";

const auto kMaxCpu = 32;

template <typename Func>
auto RunInCoro(engine::TaskProcessor& task_processor, Func&& func) {
  if (auto* task_context =
          engine::current_task::GetCurrentTaskContextUnchecked()) {
    if (&task_processor == &task_context->GetTaskProcessor())
      return func();
    else
      return engine::impl::CriticalAsync(task_processor,
                                         std::forward<Func>(func))
          .Get();
  }
  std::packaged_task<std::result_of_t<Func()>()> task(
      [&func] { return func(); });
  auto future = task.get_future();
  engine::impl::CriticalAsync(task_processor, std::move(task)).Detach();
  return future.get();
}

std::optional<size_t> GuessCpuLimit(const std::string& tp_name) {
  const char* cpu_limit_c_str = std::getenv("CPU_LIMIT");
  if (cpu_limit_c_str) {
    std::string cpu_limit(cpu_limit_c_str);
    LOG_INFO() << "CPU_LIMIT='" << cpu_limit << "'";

    try {
      size_t end{0};
      auto cpu_f = std::stod(cpu_limit, &end);
      if (cpu_limit.substr(end) == "c") {
        auto cpu = std::lround(cpu_f);
        if (cpu > 0 && cpu < kMaxCpu) {
          // TODO: hack for https://st.yandex-team.ru/TAXICOMMON-2132
          if (cpu < 3) cpu = 3;

          LOG_INFO() << "Using CPU limit from env CPU_LIMIT (" << cpu
                     << ") for worker_threads "
                     << "of task processor '" << tp_name
                     << "', ignoring config value ";
          return cpu;
        }
      }
    } catch (const std::exception& e) {
      LOG_ERROR() << "Failed to parse CPU_LIMIT: " << e;
    }
    LOG_ERROR() << "CPU_LIMIT env is invalid (" << cpu_limit
                << "), ignoring it";
  } else {
    LOG_INFO() << "CPU_LIMIT env is unset, ignoring it";
  }

  return {};
}

}  // namespace

namespace components {

Manager::TaskProcessorsStorage::TaskProcessorsStorage(
    std::shared_ptr<engine::impl::TaskProcessorPools> task_processor_pools)
    : task_processor_pools_(std::move(task_processor_pools)) {}

Manager::TaskProcessorsStorage::~TaskProcessorsStorage() {
  if (task_processor_pools_) Reset();
}

void Manager::TaskProcessorsStorage::Reset() noexcept {
  LOG_TRACE() << "Initiating task processors shutdown";
  for (auto& [name, task_processor] : task_processors_map_) {
    task_processor->InitiateShutdown();
  }
  LOG_TRACE() << "Waiting for all coroutines to become idle";
  while (task_processor_pools_->GetCoroPool().GetStats().active_coroutines) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  LOG_TRACE() << "Stopping task processors";
  task_processors_map_.clear();
  LOG_TRACE() << "Stopped task processors";
  LOG_TRACE() << "Stopping task processor pools";
  UASSERT(task_processor_pools_.use_count() == 1);
  task_processor_pools_.reset();
  LOG_TRACE() << "Stopped task processor pools";
}

void Manager::TaskProcessorsStorage::Add(
    std::string name, std::unique_ptr<engine::TaskProcessor>&& task_processor) {
  task_processors_map_.emplace(std::move(name), std::move(task_processor));
}

Manager::Manager(std::unique_ptr<ManagerConfig>&& config,
                 const ComponentList& component_list)
    : config_(std::move(config)),
      task_processors_storage_(
          std::make_shared<engine::impl::TaskProcessorPools>(
              config_->coro_pool, config_->event_thread_pool)),
      components_cleared_(false),
      default_task_processor_(nullptr),
      start_time_(std::chrono::steady_clock::now()),
      logging_component_(nullptr),
      load_duration_{0} {
  LOG_INFO() << "Starting components manager";

  for (auto processor_config : config_->task_processors) {
    if (processor_config.should_guess_cpu_limit) {
      if (config_->default_task_processor == processor_config.name) {
        auto guess_cpu = GuessCpuLimit(processor_config.name);
        if (guess_cpu) {
          processor_config.worker_threads = *guess_cpu;
        }
      } else {
        LOG_ERROR() << "guess-cpu-limit is set for non-default task processor ("
                    << processor_config.name << "), ignoring it";
      }
    }
    task_processors_storage_.Add(
        processor_config.name,
        std::make_unique<engine::TaskProcessor>(
            processor_config,
            task_processors_storage_.GetTaskProcessorPools()));
  }
  const auto& task_processors_map = task_processors_storage_.GetMap();
  const auto default_task_processor_it =
      task_processors_map.find(config_->default_task_processor);
  if (default_task_processor_it == task_processors_map.end()) {
    throw std::runtime_error(
        "Cannot start components manager: missing default task processor");
  }
  default_task_processor_ = default_task_processor_it->second.get();
  RunInCoro(*default_task_processor_, [this, &component_list]() {
    CreateComponentContext(component_list);
  });

  LOG_INFO() << "Started components manager";
}

Manager::~Manager() {
  LOG_INFO() << "Stopping components manager";
  LOG_TRACE() << "Stopping component context";
  try {
    RunInCoro(*default_task_processor_, [this]() { ClearComponents(); });
  } catch (const std::exception& exc) {
    LOG_ERROR() << "Failed to clear components: " << exc;
  }
  component_context_.reset();
  LOG_TRACE() << "Stopped component context";
  task_processors_storage_.Reset();
  LOG_INFO() << "Stopped components manager";
}

const ManagerConfig& Manager::GetConfig() const { return *config_; }

const std::shared_ptr<engine::impl::TaskProcessorPools>&
Manager::GetTaskProcessorPools() const {
  return task_processors_storage_.GetTaskProcessorPools();
}

const Manager::TaskProcessorsMap& Manager::GetTaskProcessorsMap() const {
  return task_processors_storage_.GetMap();
}

void Manager::OnLogRotate() {
  std::shared_lock<std::shared_timed_mutex> lock(context_mutex_);
  if (components_cleared_) return;
  if (logging_component_) logging_component_->OnLogRotate();
}

std::chrono::steady_clock::time_point Manager::GetStartTime() const {
  return start_time_;
}

std::chrono::milliseconds Manager::GetLoadDuration() const {
  return load_duration_;
}

void Manager::CreateComponentContext(const ComponentList& component_list) {
  std::set<std::string> loading_component_names;
  for (const auto& adder : component_list) {
    auto [it, inserted] =
        loading_component_names.insert(adder->GetComponentName());
    if (!inserted) {
      std::string message =
          "duplicate component name in component_list: " + *it;
      LOG_ERROR() << message;
      throw std::runtime_error(message);
    }
  }
  component_context_ = std::make_unique<components::ComponentContext>(
      *this, loading_component_names);

  AddComponents(component_list);
}

void Manager::AddComponents(const ComponentList& component_list) {
  components::ComponentConfigMap component_config_map;

  for (const auto& component_config : config_->components) {
    const auto& name = component_config.Name();
    if (!component_list.Contains(name)) {
      ClearComponents();
      throw std::runtime_error(
          "component config is found in config.yaml, but no component with "
          "such name is registered: '" +
          name + "', forgot to register in RegisterUserComponents()?");
    }
    component_config_map.emplace(name, component_config);
  }

  auto start_time = std::chrono::steady_clock::now();
  std::vector<engine::TaskWithResult<void>> tasks;
  bool is_load_cancelled = false;
  try {
    for (const auto& adder : component_list) {
      auto task_name = "boot/" + adder->GetComponentName();
      tasks.push_back(utils::CriticalAsync(task_name, [&]() {
        try {
          (*adder)(*this, component_config_map);
        } catch (const ComponentsLoadCancelledException& ex) {
          LOG_WARNING() << "Cannot start component "
                        << adder->GetComponentName() << ": " << ex;
          component_context_->CancelComponentsLoad();
          throw;
        } catch (const std::exception& ex) {
          LOG_ERROR() << "Cannot start component " << adder->GetComponentName()
                      << ": " << ex;
          component_context_->CancelComponentsLoad();
          throw;
        } catch (...) {
          component_context_->CancelComponentsLoad();
          throw;
        }
      }));
    }

    for (auto& task : tasks) {
      try {
        task.Get();
      } catch (const ComponentsLoadCancelledException&) {
        is_load_cancelled = true;
      }
    }
  } catch (const std::exception& ex) {
    component_context_->CancelComponentsLoad();

    /* Wait for all tasks to exit, but don't .Get() them - we've already caught
     * an exception, ignore the rest */
    for (auto& task : tasks) {
      if (task.IsValid()) task.Wait();
    }

    ClearComponents();
    throw;
  }

  if (is_load_cancelled) {
    ClearComponents();
    throw std::logic_error(
        "Components load cancelled, but only ComponentsLoadCancelledExceptions "
        "were caught");
  }

  LOG_INFO() << "All components created";
  try {
    component_context_->OnAllComponentsLoaded();
  } catch (const std::exception& ex) {
    ClearComponents();
    throw;
  }

  auto stop_time = std::chrono::steady_clock::now();
  load_duration_ = std::chrono::duration_cast<std::chrono::milliseconds>(
      stop_time - start_time);

  LOG_INFO() << "All components loaded";
}

void Manager::AddComponentImpl(
    const components::ComponentConfigMap& config_map, const std::string& name,
    std::function<std::unique_ptr<components::impl::ComponentBase>(
        const components::ComponentConfig&,
        const components::ComponentContext&)>
        factory) {
  const auto config_it = config_map.find(name);
  if (config_it == config_map.end()) {
    throw std::runtime_error("Cannot start component " + name +
                             ": missing config");
  }
  auto enabled =
      config_it->second.ParseOptionalBool("load-enabled").value_or(true);
  if (!enabled) {
    LOG_INFO() << "Component " << name
               << " load disabled in config.yaml, skipping";
    return;
  }

  LOG_INFO() << "Starting component " << name;

  auto* component = component_context_->AddComponent(
      name, [&factory, &config = config_it->second](
                const components::ComponentContext& component_context) {
        return factory(config, component_context);
      });
  if (auto* logging_component = dynamic_cast<components::Logging*>(component))
    logging_component_ = logging_component;

  LOG_INFO() << "Started component " << name;
}

void Manager::ClearComponents() noexcept {
  {
    std::unique_lock<std::shared_timed_mutex> lock(context_mutex_);
    components_cleared_ = true;
  }
  try {
    component_context_->ClearComponents();
  } catch (const std::exception& ex) {
    LOG_ERROR() << "error in clear components: " << ex;
  }
}

}  // namespace components
