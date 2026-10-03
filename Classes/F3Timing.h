// F3Timing.h -- TEMPORARY Stage F / F.3 timing hooks. NEVER COMMIT, never merge.
// Spec: WORKING/emod-format-phase0-f2-plan-2026-10-03.md §4. Saved as WORKING/f3-timing-hooks.patch.
// One "[F3] ..." line per event goes to stderr, which eden_main_native dup2's onto
// emod-last-run.log on iOS. Header-only (C++17 inline variable) so no build-list change is needed.
#ifndef F3_TIMING_H
#define F3_TIMING_H

#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

extern "C" double eden_platform_now_ms(void);

struct F3State {
    // event lifecycle
    bool active=false; char name[8]={0}; int seq=0;
    double t0=0; long frames=0; bool timedOut=false;
    // save
    double save_ms=0; int save_calls=0; int save_cols=0; double save_file_mb=0; int save_inplace=0;
    // read / decode / publish, by source
    double rd_ms=0; long rd_bytes=0; int cols_dir=0, cols_def=0, cols_gen=0, cols_empty=0;
    double dec_ms=0;      // fmh_decodeColumnBands (default-map columns only)
    double pub_ms=0;      // band copy + blockarray mirror + addChunk (dir + default columns)
    double gen_ms=0;      // generateColumn / generateEmptyColumn
    // lighting
    double lit_begin_ms=0; int lit_begin_calls=0;
    double lit_slice_ms=0; int lit_slice_calls=0; int lit_done_frame=-1; int lightboxes=0;
    // mesh
    int mesh_ne_n=0, mesh_e_n=0; double mesh_ne_ms=0, mesh_e_ms=0; long mesh_first=-1, mesh_last=-1;
    // upload (CPU submission time; GL is asynchronous)
    int up_n=0; double up_ms=0; long up_bytes=0;
    // frame view
    double worst_frame=0; int over16=0, over33=0;
    double last_update=0;
    // memory
    double mem_next=0; double mem_cur_mb=0, mem_peak_mb=0, mem_peak_ev_mb=0, mem_logged_peak=0;
};
inline F3State g_f3;

static inline double f3_now(){ return eden_platform_now_ms(); }

static inline double f3_footprint_mb(){
#if defined(__APPLE__)
    task_vm_info_data_t info; mach_msg_type_number_t cnt=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&cnt)==KERN_SUCCESS)
        return (double)info.phys_footprint/(1024.0*1024.0);
#endif
    return 0;
}

// Reset the per-event accumulators but keep global state (seq, memory peak, last_update).
static inline void f3_reset_event(){
    F3State keep=g_f3; F3State fresh;
    fresh.seq=keep.seq; fresh.last_update=keep.last_update; fresh.mem_next=keep.mem_next;
    fresh.mem_cur_mb=keep.mem_cur_mb; fresh.mem_peak_mb=keep.mem_peak_mb; fresh.mem_logged_peak=keep.mem_logged_peak;
    g_f3=fresh;
}

static inline void f3_end(bool timedOut);
static inline void f3_begin(const char* nm){
    if(g_f3.active)f3_end(true);   // an event overlapped by a new one is closed as interrupted
    f3_reset_event();
    g_f3.active=true; g_f3.seq++; snprintf(g_f3.name,sizeof g_f3.name,"%s",nm);
    g_f3.t0=f3_now(); g_f3.mem_peak_ev_mb=g_f3.mem_cur_mb;
    fprintf(stderr,"[F3] begin %s #%d\n",g_f3.name,g_f3.seq);
}

static inline void f3_end(bool timedOut){
    F3State& s=g_f3; if(!s.active)return;
    double wall=f3_now()-s.t0; s.active=false;
    int mesh_n=s.mesh_ne_n+s.mesh_e_n;
    fprintf(stderr,
      "[F3] %s #%d%s wall=%.1fms frames=%ld | save=%.1fms(calls=%d cols=%d file=%.0fMB inplace=%d) "
      "| read=%.1fms(%ldB dir=%d def=%d gen=%d empty=%d) decode=%.1fms publish=%.1fms gen=%.1fms "
      "| light=begin %.1fms(%d)+slices %.1fms(%d calls, done@frame %d, lightboxes=%d) "
      "| mesh=nonempty %d/%.1fms empty %d/%.1fms span=%ld frames (n=%d) "
      "| upload=%d/%.1fms/%ldB | frame worst=%.1fms >16.7=%d >33.3=%d | mem=%.0fMB peak(ev)=%.0fMB peak(run)=%.0fMB\n",
      s.name,s.seq,timedOut?" TIMEOUT/INTERRUPTED":"",wall,s.frames,
      s.save_ms,s.save_calls,s.save_cols,s.save_file_mb,s.save_inplace,
      s.rd_ms,s.rd_bytes,s.cols_dir,s.cols_def,s.cols_gen,s.cols_empty,s.dec_ms,s.pub_ms,s.gen_ms,
      s.lit_begin_ms,s.lit_begin_calls,s.lit_slice_ms,s.lit_slice_calls,s.lit_done_frame,s.lightboxes,
      s.mesh_ne_n,s.mesh_ne_ms,s.mesh_e_n,s.mesh_e_ms,s.mesh_first<0?0L:(s.mesh_last-s.mesh_first+1),mesh_n,
      s.up_n,s.up_ms,s.up_bytes,s.worst_frame,s.over16,s.over33,s.mem_cur_mb,s.mem_peak_ev_mb,s.mem_peak_mb);
}

// Called at the top of World::update: frame interval + memory sampling.
static inline void f3_frame_begin(){
    F3State& s=g_f3; double now=f3_now();
    if(s.active&&s.last_update>0){
        double dt=now-s.last_update;
        if(dt>s.worst_frame)s.worst_frame=dt;
        if(dt>16.7)s.over16++;
        if(dt>33.3)s.over33++;
    }
    s.last_update=now;
    if(now>=s.mem_next){
        s.mem_next=now+1000.0;
        s.mem_cur_mb=f3_footprint_mb();
        if(s.mem_cur_mb>s.mem_peak_mb)s.mem_peak_mb=s.mem_cur_mb;
        if(s.active&&s.mem_cur_mb>s.mem_peak_ev_mb)s.mem_peak_ev_mb=s.mem_cur_mb;
        if(s.mem_peak_mb>=s.mem_logged_peak+16.0){
            s.mem_logged_peak=s.mem_peak_mb;
            fprintf(stderr,"[F3] mem cur=%.0fMB peak=%.0fMB\n",s.mem_cur_mb,s.mem_peak_mb);
        }
    }
}

// Scope timer: adds elapsed ms to *acc on destruction. Costs nothing when no event is active
// beyond two clock reads.
struct F3Scope {
    double* acc; double t;
    explicit F3Scope(double* a):acc(a),t(f3_now()){}
    ~F3Scope(){ *acc+=f3_now()-t; }
};

#endif
