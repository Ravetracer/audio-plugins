// Renders RainyDay preset files back to back inside one process, so a fitting
// loop is not paying dlopen + activate for every candidate.
//
//   fithost <plugin.clap>
//   stdin :  "<preset path>\t<seconds>\n"  per render, "quit\n" to stop
//   stdout:  uint32 little-endian frame count, then that many float32 mono samples
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
#include <clap/clap.h>

static const void *hostExt(const clap_host_t *, const char *) { return nullptr; }
static void nop(const clap_host_t *) {}

int main(int argc, char **argv) {
   const char *path = argc > 1 ? argv[1] : "./RainyDay.clap";
   void *dso = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!dso) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
   auto *entry = (const clap_plugin_entry_t *)dlsym(dso, "clap_entry");
   if (!entry || !entry->init(path)) { std::fprintf(stderr, "entry init failed\n"); return 1; }
   auto *fac = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
   const clap_plugin_descriptor_t *desc = fac->get_plugin_descriptor(fac, 0);

   clap_host_t host = {CLAP_VERSION_INIT, nullptr, "fithost", "x", "x", "1", hostExt, nop, nop, nop};
   const clap_plugin_t *plug = fac->create_plugin(fac, &host, desc->id);
   plug->init(plug);

   const double sr = 48000.0;
   const uint32_t block = 512;
   plug->activate(plug, sr, block, block);
   auto *pl = (const clap_plugin_preset_load_t *)plug->get_extension(plug, CLAP_EXT_PRESET_LOAD);

   std::vector<float> left(block), right(block);
   float *chans[2] = {left.data(), right.data()};

   char line[4096];
   while (std::fgets(line, sizeof(line), stdin)) {
      if (!std::strncmp(line, "quit", 4)) break;
      char *tab = std::strchr(line, '\t');
      if (!tab) continue;
      *tab = 0;
      const double seconds = std::atof(tab + 1);

      // A fresh reset per render, so one candidate cannot bleed into the next.
      plug->deactivate(plug);
      plug->activate(plug, sr, block, block);
      if (!pl->from_location(plug, CLAP_PRESET_DISCOVERY_LOCATION_FILE, line, nullptr)) {
         std::fprintf(stderr, "preset load failed: %s\n", line);
         uint32_t zero = 0; std::fwrite(&zero, 4, 1, stdout); std::fflush(stdout); continue;
      }
      plug->reset(plug);
      plug->start_processing(plug);

      const uint32_t total = (uint32_t)(seconds * sr);
      std::vector<float> mono; mono.reserve(total);

      clap_event_note_t on{};
      on.header.size = sizeof(on); on.header.type = CLAP_EVENT_NOTE_ON;
      on.header.space_id = CLAP_CORE_EVENT_SPACE_ID; on.header.time = 0;
      on.note_id = 1; on.port_index = 0; on.channel = 0; on.key = 60; on.velocity = 1.0;
      static const clap_event_header_t *pending;
      pending = &on.header;

      clap_input_events_t inFirst{}, inRest{};
      inFirst.ctx = nullptr;
      inFirst.size = [](const clap_input_events_t *) -> uint32_t { return 1; };
      inFirst.get = [](const clap_input_events_t *, uint32_t) { return pending; };
      inRest.ctx = nullptr;
      inRest.size = [](const clap_input_events_t *) -> uint32_t { return 0; };
      inRest.get = [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return nullptr; };
      clap_output_events_t out{};
      out.ctx = nullptr;
      out.try_push = [](const clap_output_events_t *, const clap_event_header_t *) -> bool { return true; };

      clap_audio_buffer_t ab{}; ab.data32 = chans; ab.channel_count = 2;
      bool first = true;
      for (uint32_t done = 0; done < total; done += block) {
         clap_process_t pr{};
         pr.frames_count = block;
         pr.audio_outputs = &ab; pr.audio_outputs_count = 1;
         pr.in_events = first ? &inFirst : &inRest;
         pr.out_events = &out;
         first = false;
         plug->process(plug, &pr);
         for (uint32_t i = 0; i < block && done + i < total; ++i)
            mono.push_back(0.5f * (left[i] + right[i]));
      }
      plug->stop_processing(plug);

      const uint32_t n = (uint32_t)mono.size();
      std::fwrite(&n, 4, 1, stdout);
      std::fwrite(mono.data(), 4, n, stdout);
      std::fflush(stdout);
   }

   plug->deactivate(plug);
   plug->destroy(plug);
   entry->deinit();
   return 0;
}
