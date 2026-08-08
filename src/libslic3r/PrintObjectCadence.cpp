#include "Print.hpp"

#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "PrintConfig.hpp"
#include "SurfaceCollection.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace Slic3r {

namespace {

struct CadenceGroup
{
    size_t first;
    size_t last;
    double thickness;
};

Surfaces copy_surfaces(const SurfacesPtr &surfaces)
{
    Surfaces copies;
    copies.reserve(surfaces.size());
    for (const Surface *surface : surfaces)
        copies.emplace_back(*surface);
    return copies;
}

Polygons expanded(const ExPolygons &expolygons, float offset_distance)
{
    Polygons result;
    result.reserve(expolygons.size());
    for (const ExPolygon &expolygon : expolygons)
        polygons_append(result, offset(expolygon, offset_distance));
    return result;
}

double effective_thickness(const Surface &surface, const Layer &layer)
{
    return surface.thickness < 0. ? layer.height : surface.thickness;
}

void append_with_thickness(SurfaceCollection &collection, ExPolygons expolygons, const Surface &source,
                           double thickness, unsigned short thickness_layers)
{
    if (expolygons.empty())
        return;
    Surface templ(source);
    templ.thickness        = thickness;
    templ.thickness_layers = thickness_layers;
    collection.append(std::move(expolygons), templ);
}

double feature_height_cap(const PrintConfig &print_config, unsigned int filament_id, double base_height)
{
    const size_t tool_id = get_extruder_index_from_filament_id(print_config, filament_id);
    const double nozzle  = print_config.nozzle_diameter.get_at(tool_id);
    double max_height    = print_config.max_layer_height.get_at(tool_id);
    if (max_height <= 0.)
        max_height = 0.75 * nozzle;
    return std::min({base_height, nozzle, max_height});
}

void consume_internal_solid(LayerRegion &layerm, const ExPolygons &footprint, float clearance_offset)
{
    const Polygons solids = to_polygons(layerm.fill_surfaces.filter_by_type(stInternalSolid));
    if (solids.empty())
        return;

    const Polygons with_clearance = expanded(footprint, clearance_offset);
    layerm.fill_surfaces.remove_type(stInternalSolid);
    layerm.fill_surfaces.append(diff_ex(solids, with_clearance), stInternalSolid);
    layerm.fill_surfaces.append(intersection_ex(solids, with_clearance), stInternalVoid);
}

} // namespace

float PrintObject::infill_combination_clearance(const LayerRegion &layerm, InfillPattern infill_pattern)
{
    return 0.5f * layerm.flow(frPerimeter).scaled_width() +
           // These patterns are grown later to overlap perimeters, so counteract that too.
           ((infill_pattern == ipRectilinear || infill_pattern == ipMonotonic ||
             infill_pattern == ipGrid || infill_pattern == ipLateralLattice ||
             infill_pattern == ipLine || infill_pattern == ipHoneycomb ||
             infill_pattern == ipLateralHoneycomb) ? 1.5f : 0.5f) *
               layerm.flow(frSolidInfill).scaled_width();
}

void PrintObject::recombine_feature_cadence()
{
    if (m_slicing_params.cadence_ratio <= 1 || m_layers.size() < 2)
        return;

    const double base_height = m_slicing_params.base_layer_height;
    std::vector<CadenceGroup> groups;
    groups.reserve(m_layers.size() / size_t(m_slicing_params.cadence_ratio));

    // The first object layer has its own configured height and is never recombined.
    size_t first = 1;
    while (first < m_layers.size()) {
        double thickness = 0.;
        size_t last = first;
        for (; last < m_layers.size() && thickness < base_height - EPSILON; ++last)
            thickness += m_layers[last]->height;

        if (std::abs(thickness - base_height) <= EPSILON) {
            groups.push_back({first, last - 1, thickness});
            first = last;
        } else if (last == m_layers.size()) {
            // A partial group at the object top remains on the fine grid.
            break;
        } else {
            // P1.4 produces aligned groups. If that invariant is ever broken, skip rather than
            // combining a geometrically incorrect height.
            assert(thickness <= base_height + EPSILON);
            first = last;
        }
    }

    const PrintConfig &print_config = this->print()->config();
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
        const PrintRegionConfig &region_config = this->printing_region(region_id).config();

        for (const CadenceGroup &group : groups) {
            m_print->throw_if_canceled();
            if (group.first == group.last)
                continue;

            // Top surfaces remain on their original layer and grow downward through solid infill.
            // Tops and bottoms absorb fine solids BEFORE the interior pass combines the remainder:
            // absorbing from an already-combined surface would flatten its thickness metadata.
            const double top_cap = feature_height_cap(
                print_config, region_config.top_surface_filament_id.value, base_height);
            for (size_t layer_idx = group.first; layer_idx <= group.last; ++layer_idx) {
                LayerRegion *top_layerm = m_layers[layer_idx]->regions()[region_id];
                Surfaces tops = copy_surfaces(top_layerm->fill_surfaces.filter_by_type(stTop));
                if (tops.empty())
                    continue;
                top_layerm->fill_surfaces.remove_type(stTop);

                for (const Surface &top : tops) {
                    ExPolygons extending{top.expolygon};
                    double thickness = effective_thickness(top, *m_layers[layer_idx]);
                    unsigned short thickness_layers = top.thickness_layers;

                    for (size_t lower_idx = layer_idx; lower_idx-- > group.first;) {
                        if (thickness + m_layers[lower_idx]->height > top_cap + EPSILON)
                            break;
                        LayerRegion *lower = m_layers[lower_idx]->regions()[region_id];
                        ExPolygons absorb = intersection_ex(
                            lower->fill_surfaces.filter_by_type(stInternalSolid), extending);
                        if (absorb.empty())
                            break;

                        append_with_thickness(top_layerm->fill_surfaces, diff_ex(extending, absorb), top,
                                              thickness, thickness_layers);
                        consume_internal_solid(*lower, absorb,
                            infill_combination_clearance(*lower, region_config.internal_solid_infill_pattern.value));
                        extending = std::move(absorb);
                        thickness += m_layers[lower_idx]->height;
                        ++thickness_layers;
                    }
                    append_with_thickness(top_layerm->fill_surfaces, std::move(extending), top,
                                          thickness, thickness_layers);
                }
            }

            // Bottom surfaces grow upward. Each absorbed partition moves to the layer whose print_z
            // makes the taller extrusion's bottom coincide with the original bottom face.
            std::vector<Surfaces> bottoms(group.last - group.first + 1);
            for (size_t layer_idx = group.first; layer_idx <= group.last; ++layer_idx) {
                LayerRegion *layerm = m_layers[layer_idx]->regions()[region_id];
                bottoms[layer_idx - group.first] = copy_surfaces(layerm->fill_surfaces.filter_by_type(stBottom));
                layerm->fill_surfaces.remove_type(stBottom);
            }

            const double bottom_cap = feature_height_cap(
                print_config, region_config.bottom_surface_filament_id.value, base_height);
            for (size_t layer_idx = group.first; layer_idx <= group.last; ++layer_idx) {
                LayerRegion *source = m_layers[layer_idx]->regions()[region_id];
                for (const Surface &bottom : bottoms[layer_idx - group.first]) {
                    ExPolygons extending{bottom.expolygon};
                    ExPolygons moved_from_source;
                    double thickness = effective_thickness(bottom, *m_layers[layer_idx]);
                    unsigned short thickness_layers = bottom.thickness_layers;
                    size_t destination_idx = layer_idx;

                    for (size_t upper_idx = layer_idx + 1; upper_idx <= group.last; ++upper_idx) {
                        if (thickness + m_layers[upper_idx]->height > bottom_cap + EPSILON)
                            break;
                        LayerRegion *upper = m_layers[upper_idx]->regions()[region_id];
                        ExPolygons absorb = intersection_ex(
                            upper->fill_surfaces.filter_by_type(stInternalSolid), extending);
                        if (absorb.empty())
                            break;

                        append_with_thickness(m_layers[destination_idx]->regions()[region_id]->fill_surfaces,
                                              diff_ex(extending, absorb), bottom, thickness, thickness_layers);
                        if (destination_idx == layer_idx)
                            moved_from_source = absorb;
                        consume_internal_solid(*upper, absorb,
                            infill_combination_clearance(*upper, region_config.internal_solid_infill_pattern.value));
                        extending = std::move(absorb);
                        thickness += m_layers[upper_idx]->height;
                        ++thickness_layers;
                        destination_idx = upper_idx;
                    }

                    append_with_thickness(m_layers[destination_idx]->regions()[region_id]->fill_surfaces,
                                          std::move(extending), bottom, thickness, thickness_layers);
                    source->fill_surfaces.append(std::move(moved_from_source), stInternalVoid);
                }
            }

            // Recombine the remaining sparse and solid interiors only where the full base group
            // shares a footprint. Runs after tops/bottoms so absorbed areas are already voids.
            for (SurfaceType type : {stInternal, stInternalSolid}) {
                const unsigned int filament_id = type == stInternal ? region_config.sparse_infill_filament_id.value :
                                                                     region_config.internal_solid_filament_id.value;
                if (group.thickness > feature_height_cap(print_config, filament_id, base_height) + EPSILON)
                    continue;

                LayerRegion *top_layerm = m_layers[group.last]->regions()[region_id];
                ExPolygons intersection = to_expolygons(top_layerm->fill_surfaces.filter_by_type(type));
                for (size_t layer_idx = group.last; !intersection.empty() && layer_idx-- > group.first;)
                    intersection = intersection_ex(
                        m_layers[layer_idx]->regions()[region_id]->fill_surfaces.filter_by_type(type), intersection);

                const double area_threshold = m_layers[group.first]->regions()[region_id]->infill_area_threshold();
                if (area_threshold > 0.)
                    intersection.erase(std::remove_if(intersection.begin(), intersection.end(),
                        [area_threshold](const ExPolygon &expolygon) { return expolygon.area() <= area_threshold; }),
                        intersection.end());
                if (intersection.empty())
                    continue;

                const InfillPattern pattern = type == stInternal ? region_config.sparse_infill_pattern.value :
                                                                  region_config.internal_solid_infill_pattern.value;
                const float clearance_offset = infill_combination_clearance(*top_layerm, pattern);
                const Polygons intersection_with_clearance = expanded(intersection, clearance_offset);

                for (size_t layer_idx = group.first; layer_idx <= group.last; ++layer_idx) {
                    LayerRegion *layerm = m_layers[layer_idx]->regions()[region_id];
                    const Polygons original = to_polygons(layerm->fill_surfaces.filter_by_type(type));
                    layerm->fill_surfaces.remove_type(type);
                    layerm->fill_surfaces.append(diff_ex(original, intersection_with_clearance), type);
                    if (layer_idx == group.last) {
                        Surface templ(type, ExPolygon());
                        templ.thickness        = group.thickness;
                        templ.thickness_layers = static_cast<unsigned short>(group.last - group.first + 1);
                        layerm->fill_surfaces.append(intersection, templ);
                    } else {
                        layerm->fill_surfaces.append(
                            intersection_ex(original, intersection_with_clearance), stInternalVoid);
                    }
                }
            }
        }
    }
}

} // namespace Slic3r
