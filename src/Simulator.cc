#include "Simulator.h"

#include <filesystem>
#include <string>

#include "SystolicOS.h"
#include "SystolicWS.h"

namespace fs = std::filesystem;

Simulator::Simulator(SimulationConfig config)
    : _config(config), _core_cycles(0) {
  // Create dram object
  _core_period = 1000000 / (config.core_freq);
  _icnt_period = 1000000 / (config.icnt_freq);
  _dram_period = 1000000 / (config.dram_freq);
  _core_time = 0;
  _dram_time = 0;
  _icnt_time = 0;
  if (config.dram_type == DramType::SIMPLE) {
    _dram = std::make_unique<SimpleDram>(config);
  } else if (config.dram_type == DramType::RAMULATOR) {
    char* onnxim_path_env = std::getenv("ONNXIM_HOME");
    std::string onnxim_path = onnxim_path_env != NULL?
      std::string(onnxim_path_env) : std::string("./");
    std::string ramulator_config = fs::path(onnxim_path)
                                       .append("configs")
                                       .append(config.dram_config_path)
                                       .string();
    spdlog::info("Ramulator config: {}", ramulator_config);
    config.dram_config_path = ramulator_config;
    _dram = std::make_unique<DramRamulator>(config);
  } else {
    spdlog::error("[Configuration] Invalid DRAM type...!");
    exit(EXIT_FAILURE);
  }

  // Create interconnect object
  if (config.icnt_type == IcntType::SIMPLE) {
    _icnt = std::make_unique<SimpleInterconnect>(config);
  } else if (config.icnt_type == IcntType::BOOKSIM2) {
    _icnt = std::make_unique<Booksim2Interconnect>(config);
  } else {
    spdlog::error("[Configuration] {} Invalid interconnect type...!");
    exit(EXIT_FAILURE);
  }

  // Create core objects
  _cores.resize(config.num_cores);
  _n_cores = config.num_cores;
  _n_memories = config.dram_channels;
  for (int core_index = 0; core_index < _n_cores; core_index++) {
    if (config.core_type == CoreType::SYSTOLIC_OS)
      _cores[core_index] = std::make_unique<SystolicOS>(core_index, _config);
    else if (config.core_type == CoreType::SYSTOLIC_WS)
      _cores[core_index] = std::make_unique<SystolicWS>(core_index, _config);
    else {
      spdlog::error("[Configuration] Invalid core type...!");
      exit(EXIT_FAILURE);
    }
  }
  
  if (config.scheduler_type == "simple") {
    _scheduler = std::make_unique<Scheduler>(_config, &_core_cycles, &_core_time);
  } else if (config.scheduler_type == "partition_cpu") {
    _scheduler =
        std::make_unique<DedicatedCPUScheduler>(_config, &_core_cycles, &_core_time);
  } else if (config.scheduler_type == "time_multiplex") {
    _scheduler =
        std::make_unique<TimeMultiplexScheduler>(_config, &_core_cycles, &_core_time);
  } else if (config.scheduler_type == "spatial_split") {
    _scheduler = std::make_unique<HalfSplitScheduler>(_config, &_core_cycles, &_core_time);
  } else {
    spdlog::error("[Configuration] {} is invalid scheduler type...!", config.scheduler_type);
    exit(EXIT_FAILURE);
  }

  /* Create heap */
  std::make_heap(_models.begin(), _models.end(), CompareModel());
}

void Simulator::run_simulator() {
  spdlog::info("======Start Simulation=====");
  cycle();
}

void Simulator::handle_model() {
  while (!_models.empty() && _models.front()->get_request_time() <= _core_time) {
    std::unique_ptr<Model> launch_model = std::move(_models.front());
    std::pop_heap(_models.begin(), _models.end(), CompareModel());
    _models.pop_back();

    launch_model->initialize_model();
    launch_model->set_request_time(_core_time);
    spdlog::info("Schedule model: {} at {} us", launch_model->get_name(), _core_time / (1000000));
    _scheduler->schedule_model(std::move(launch_model), 1);
  }
}

void Simulator::cycle() {
  OpStat op_stat;
  ModelStat model_stat;
  uint32_t tile_count;
  bool is_accum_tile;
  while (running()) {
    int model_id = 0;

    set_cycle_mask();
    // Core Cycle
    if (_cycle_mask & CORE_MASK) {
      /* Handle requested model */
      handle_model();

      for (int core_id = 0; core_id < _n_cores; core_id++) {
        std::unique_ptr<Tile> finished_tile = _cores[core_id]->pop_finished_tile();
        if (finished_tile->status == Tile::Status::FINISH) {
          _scheduler->finish_tile(core_id, finished_tile->layer_id);
        }
        // Issue new tile to core
        if (!_scheduler->empty()) {
          is_accum_tile = _scheduler->is_accum_tile(core_id, 0);
          if (_cores[core_id]->can_issue(is_accum_tile)) {
            std::unique_ptr<Tile> tile = _scheduler->get_tile(core_id);
            if (tile->status == Tile::Status::INITIALIZED) {
              _cores[core_id]->issue(std::move(tile));
            }
          }
        }
        _cores[core_id]->cycle();
      }
      _core_cycles++;
    }

    // DRAM cycle
    if (_cycle_mask & DRAM_MASK) {
      _dram->cycle();
    }
    // Interconnect cycle
    if (_cycle_mask & ICNT_MASK) {
      for (int core_id = 0; core_id < _n_cores; core_id++) {
        // PUHS core to ICNT. memory request
        if (_cores[core_id]->has_memory_request()) {
          MemoryAccess *front = _cores[core_id]->top_memory_request();
          front->core_id = core_id;
          if (!_icnt->is_full(core_id, front)) {
            _icnt->push(core_id, get_dest_node(front), front);
            _cores[core_id]->pop_memory_request();
          }
        }
        // Push response from ICNT. to Core.
        if (!_icnt->is_empty(core_id)) {
          _cores[core_id]->push_memory_response(_icnt->top(core_id));
          _icnt->pop(core_id);
        }
      }

      for (int mem_id = 0; mem_id < _n_memories; mem_id++) {
        // ICNT to memory
        if (!_icnt->is_empty(_n_cores + mem_id) &&
            !_dram->is_full(mem_id, _icnt->top(_n_cores + mem_id))) {
          _dram->push(mem_id, _icnt->top(_n_cores + mem_id));
          _icnt->pop(_n_cores + mem_id);
        }
        // Pop response to ICNT from dram
        if (!_dram->is_empty(mem_id) &&
            !_icnt->is_full(_n_cores + mem_id, _dram->top(mem_id))) {
          _icnt->push(_n_cores + mem_id, get_dest_node(_dram->top(mem_id)),
                      _dram->top(mem_id));
          _dram->pop(mem_id);
        }
      }

      _icnt->cycle();
    }
  }
  spdlog::info("Simulation Finished");
  /* Print simulation stats */
  for (int core_id = 0; core_id < _n_cores; core_id++) {
    _cores[core_id]->print_stats();
  }
  _icnt->print_stats();
  _dram->print_stat();
}

void Simulator::register_model(std::unique_ptr<Model> model) {
  _models.push_back(std::move(model));
  std::push_heap(_models.begin(), _models.end(), CompareModel());
}

bool Simulator::running() {
  bool running = false;
  running |= !_models.empty();
  for (auto &core : _cores) {
    running = running || core->running();
  }
  running = running || _icnt->running();
  running = running || _dram->running();
  running = running || !_scheduler->empty();
  return running;
}

void Simulator::set_cycle_mask() {
  _cycle_mask = 0x0;
  uint64_t minimum_time = MIN3(_core_time, _dram_time, _icnt_time);
  if (_core_time <= minimum_time) {
    _cycle_mask |= CORE_MASK;
    _core_time += _core_period;
  }
  if (_dram_time <= minimum_time) {
    _cycle_mask |= DRAM_MASK;
    _dram_time += _dram_period;
  }
  if (_icnt_time <= minimum_time) {
    _cycle_mask |= ICNT_MASK;
    _icnt_time += _icnt_period;
  }
}

uint32_t Simulator::get_dest_node(MemoryAccess *access) {
  if (access->request) {
    return _config.num_cores + _dram->get_channel_id(access);
  } else {
    return access->core_id;
  }
}