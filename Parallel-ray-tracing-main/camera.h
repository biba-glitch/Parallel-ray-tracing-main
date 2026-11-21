#ifndef CAMERA_H
#define CAMERA_H
//==============================================================================================
// Originally written in 2016 by Peter Shirley <ptrshrl@gmail.com>
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//
// You should have received a copy (see file COPYING.txt) of the CC0 Public Domain Dedication
// along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
//==============================================================================================
// ARTI 503 Assignment 2 - parallel version (OpenMP).
// The render loop now runs on several threads. To study race conditions, the threads also
// update two SHARED counters:
//   [R1] pixels_done  - finished pixels  (correct answer: 400 x 225 = 90,000)
//   [R2] samples_done - finished samples (correct answer: 90,000 x 10 = 900,000)
//   [R3] the random number generator (rtweekend.h) - fixed with thread_local
// The "version" setting chooses how those shared counters are protected:
//   seq | race | critical | atomic | reduction | lock
//==============================================================================================

#include "hittable.h"
#include "material.h"

#include <omp.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

class camera {
  public:
    double aspect_ratio      = 1.0;  // Ratio of image width over height
    int    image_width       = 100;  // Rendered image width in pixel count
    int    samples_per_pixel = 10;   // Count of random samples for each pixel
    int    max_depth         = 10;   // Maximum number of ray bounces into scene

    double vfov     = 90;              // Vertical view angle (field of view)
    point3 lookfrom = point3(0,0,0);   // Point camera is looking from
    point3 lookat   = point3(0,0,-1);  // Point camera is looking at
    vec3   vup      = vec3(0,1,0);     // Camera-relative "up" direction

    double defocus_angle = 0;  // Variation angle of rays through each pixel
    double focus_dist = 10;    // Distance to plane of perfect focus

    std::string version = "seq";  // NEW: which version to run
    int         threads = 1;      // NEW: how many threads to use

    void render(const hittable& world) {
        initialize();
        omp_set_num_threads(threads);

        // Allocate image storage outside the timed section.
        std::vector<color> image(
            static_cast<std::size_t>(image_width) * image_height
        );

        // NEW: shared counters - every thread adds to these.
        long long pixels_done  = 0;   // [R1] SHARED: correct answer = width x height
        long long samples_done = 0;   // [R2] SHARED: correct answer = width x height x samples

        omp_lock_t lock;              // NEW: the "key" for the lock version
        omp_init_lock(&lock);

        const auto start = std::chrono::steady_clock::now();

        if (version == "seq") {
            // Sequential: the original loop, one thread.
            for (int j = 0; j < image_height; j++) {
                set_random_seed(1000 + j);
                for (int i = 0; i < image_width; i++) {
                    color pixel_color(0,0,0);
                    for (int sample = 0; sample < samples_per_pixel; sample++) {
                        pixel_color += ray_color(get_ray(i, j), max_depth, world);
                        samples_done++;
                    }
                    image[static_cast<std::size_t>(j) * image_width + i]
                        = pixel_samples_scale * pixel_color;
                    pixels_done++;
                }
            }
        }
        else if (version == "race") {
            // Parallel, NO protection: the race condition.
            #pragma omp parallel for schedule(dynamic, 1)
            for (int j = 0; j < image_height; j++) {
                set_random_seed(1000 + j);
                for (int i = 0; i < image_width; i++) {
                    color pixel_color(0,0,0);
                    for (int sample = 0; sample < samples_per_pixel; sample++) {
                        pixel_color += ray_color(get_ray(i, j), max_depth, world);
                        samples_done++;            // <-- [R2] RACE
                    }
                    image[static_cast<std::size_t>(j) * image_width + i]
                        = pixel_samples_scale * pixel_color;
                    pixels_done++;                 // <-- [R1] RACE
                }
            }
        }
        else if (version == "critical") {
            // Fix 1: only one thread at a time may enter a critical block.
            #pragma omp parallel for schedule(dynamic, 1)
            for (int j = 0; j < image_height; j++) {
                set_random_seed(1000 + j);
                for (int i = 0; i < image_width; i++) {
                    color pixel_color(0,0,0);
                    for (int sample = 0; sample < samples_per_pixel; sample++) {
                        pixel_color += ray_color(get_ray(i, j), max_depth, world);
                        #pragma omp critical
                        samples_done++;
                    }
                    image[static_cast<std::size_t>(j) * image_width + i]
                        = pixel_samples_scale * pixel_color;
                    #pragma omp critical
                    pixels_done++;
                }
            }
        }
        else if (version == "atomic") {
            // Fix 2: each update is done by the CPU in one step that cannot be split.
            #pragma omp parallel for schedule(dynamic, 1)
            for (int j = 0; j < image_height; j++) {
                set_random_seed(1000 + j);
                for (int i = 0; i < image_width; i++) {
                    color pixel_color(0,0,0);
                    for (int sample = 0; sample < samples_per_pixel; sample++) {
                        pixel_color += ray_color(get_ray(i, j), max_depth, world);
                        #pragma omp atomic
                        samples_done++;
                    }
                    image[static_cast<std::size_t>(j) * image_width + i]
                        = pixel_samples_scale * pixel_color;
                    #pragma omp atomic
                    pixels_done++;
                }
            }
        }
        else if (version == "reduction") {
            // Fix 3: each thread counts in its own private copy; copies are added at the end.
            #pragma omp parallel for schedule(dynamic, 1) reduction(+ : pixels_done, samples_done)
            for (int j = 0; j < image_height; j++) {
                set_random_seed(1000 + j);
                for (int i = 0; i < image_width; i++) {
                    color pixel_color(0,0,0);
                    for (int sample = 0; sample < samples_per_pixel; sample++) {
                        pixel_color += ray_color(get_ray(i, j), max_depth, world);
                        samples_done++;            // private copy: safe
                    }
                    image[static_cast<std::size_t>(j) * image_width + i]
                        = pixel_samples_scale * pixel_color;
                    pixels_done++;                 // private copy: safe
                }
            }
        }
        else if (version == "lock") {
            // Fix 4: a thread must take the lock before updating and release it after.
            #pragma omp parallel for schedule(dynamic, 1)
            for (int j = 0; j < image_height; j++) {
                set_random_seed(1000 + j);
                for (int i = 0; i < image_width; i++) {
                    color pixel_color(0,0,0);
                    for (int sample = 0; sample < samples_per_pixel; sample++) {
                        pixel_color += ray_color(get_ray(i, j), max_depth, world);
                        omp_set_lock(&lock);
                        samples_done++;
                        omp_unset_lock(&lock);
                    }
                    image[static_cast<std::size_t>(j) * image_width + i]
                        = pixel_samples_scale * pixel_color;
                    omp_set_lock(&lock);
                    pixels_done++;
                    omp_unset_lock(&lock);
                }
            }
        }
        else {
            std::clog << "Unknown version '" << version
                      << "'. Use: seq, race, critical, atomic, reduction, lock\n";
            omp_destroy_lock(&lock);
            return;
        }

        const auto end = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(end - start).count();
        omp_destroy_lock(&lock);

        // NEW: the correct answers are known in advance.
        const long long correct_pixels  = static_cast<long long>(image_width) * image_height;
        const long long correct_samples = correct_pixels * samples_per_pixel;
        const bool ok = (pixels_done == correct_pixels) && (samples_done == correct_samples);
        const int used_threads = (version == "seq") ? 1 : threads;

        // Image output is excluded from the rendering time.
        std::cout << "P3\n"
                  << image_width << ' ' << image_height
                  << "\n255\n";

        for (const color& pixel : image) {
            write_color(std::cout, pixel);
        }

        // Timing and results go to stderr, separately from the image output.
        char line[400];
        std::snprintf(line, sizeof line,
            "Version: %s | Threads: %d | Render time: %.5f seconds\n"
            "  pixels_done  = %lld   (correct: %lld, lost: %lld)\n"
            "  samples_done = %lld   (correct: %lld, lost: %lld)\n"
            "  RESULT: %s\n",
            version.c_str(), used_threads, seconds,
            pixels_done, correct_pixels, correct_pixels - pixels_done,
            samples_done, correct_samples, correct_samples - samples_done,
            ok ? "CORRECT" : "WRONG");
        std::clog << line;

        // One short line for the results file (used by run_all.ps1).
        std::snprintf(line, sizeof line, "CSV,%s,%d,%.5f,%lld,%lld,%lld,%lld,%lld,%lld,%s\n",
            version.c_str(), used_threads, seconds,
            pixels_done, correct_pixels, correct_pixels - pixels_done,
            samples_done, correct_samples, correct_samples - samples_done,
            ok ? "CORRECT" : "WRONG");
        std::clog << line;
        std::clog << "Done.\n";
    }

  private:
    int    image_height;         // Rendered image height
    double pixel_samples_scale;  // Color scale factor for pixel samples
    point3 center;               // Camera center
    point3 pixel00_loc;          // Location of pixel 0, 0
    vec3   pixel_delta_u;        // Offset to pixel to the right
    vec3   pixel_delta_v;        // Offset to pixel below
    vec3   u, v, w;              // Camera frame basis vectors
    vec3   defocus_disk_u;       // Defocus disk horizontal radius
    vec3   defocus_disk_v;       // Defocus disk vertical radius

    void initialize() {
        image_height = int(image_width / aspect_ratio);
        image_height = (image_height < 1) ? 1 : image_height;

        pixel_samples_scale = 1.0 / samples_per_pixel;

        center = lookfrom;

        // Determine viewport dimensions.
        auto theta = degrees_to_radians(vfov);
        auto h = std::tan(theta/2);
        auto viewport_height = 2 * h * focus_dist;
        auto viewport_width =
            viewport_height * (double(image_width)/image_height);

        // Calculate the camera coordinate frame.
        w = unit_vector(lookfrom - lookat);
        u = unit_vector(cross(vup, w));
        v = cross(w, u);

        // Calculate the vectors across the viewport edges.
        vec3 viewport_u = viewport_width * u;
        vec3 viewport_v = viewport_height * -v;

        // Calculate the pixel-to-pixel delta vectors.
        pixel_delta_u = viewport_u / image_width;
        pixel_delta_v = viewport_v / image_height;

        // Calculate the location of the upper-left pixel.
        auto viewport_upper_left =
            center - (focus_dist * w) - viewport_u/2 - viewport_v/2;

        pixel00_loc =
            viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);

        // Calculate the camera defocus disk basis vectors.
        auto defocus_radius =
            focus_dist * std::tan(degrees_to_radians(defocus_angle / 2));

        defocus_disk_u = u * defocus_radius;
        defocus_disk_v = v * defocus_radius;
    }

    ray get_ray(int i, int j) const {
        // Construct a camera ray through a randomly sampled pixel point.
        auto offset = sample_square();
        auto pixel_sample = pixel00_loc
                          + ((i + offset.x()) * pixel_delta_u)
                          + ((j + offset.y()) * pixel_delta_v);

        auto ray_origin =
            (defocus_angle <= 0) ? center : defocus_disk_sample();

        auto ray_direction = pixel_sample - ray_origin;

        return ray(ray_origin, ray_direction);
    }

    vec3 sample_square() const {
        // Return a random point in the unit square centered at the origin.
        return vec3(random_double() - 0.5, random_double() - 0.5, 0);
    }

    vec3 sample_disk(double radius) const {
        return radius * random_in_unit_disk();
    }

    point3 defocus_disk_sample() const {
        auto p = random_in_unit_disk();
        return center + (p[0] * defocus_disk_u) + (p[1] * defocus_disk_v);
    }

    color ray_color(const ray& r, int depth, const hittable& world) const {
        // Stop when the ray bounce limit is reached.
        if (depth <= 0)
            return color(0,0,0);

        hit_record rec;

        if (world.hit(r, interval(0.001, infinity), rec)) {
            ray scattered;
            color attenuation;

            if (rec.mat->scatter(r, rec, attenuation, scattered))
                return attenuation * ray_color(scattered, depth-1, world);

            return color(0,0,0);
        }

        vec3 unit_direction = unit_vector(r.direction());
        auto a = 0.5 * (unit_direction.y() + 1.0);

        return (1.0-a) * color(1.0, 1.0, 1.0)
             + a * color(0.5, 0.7, 1.0);
    }
};

#endif
