#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Dev/Debug/OverlayScissor.hpp>

namespace
{
using Poseidon::Dev::ComputeOverlayScissor;
using Poseidon::Dev::OverlayClipRect;
using Poseidon::Dev::OverlayDisplayMetrics;
using Poseidon::Dev::OverlayScissor;

const OverlayDisplayMetrics kUnitMetrics{0.0f, 0.0f, 1.0f, 1.0f, 100, 80};
} // namespace

TEST_CASE("Overlay clip rectangles convert to bounded framebuffer scissors", "[dev][overlay][scissor]")
{
    OverlayScissor scissor{};

    SECTION("a rectangle fully inside the framebuffer passes through")
    {
        REQUIRE(ComputeOverlayScissor({10.0f, 12.0f, 70.0f, 60.0f}, kUnitMetrics, scissor));
        CHECK(scissor.x == 10);
        CHECK(scissor.y == 12);
        CHECK(scissor.width == 60);
        CHECK(scissor.height == 48);
    }

    SECTION("all four edges clamp to the framebuffer")
    {
        REQUIRE(ComputeOverlayScissor({-10.0f, -20.0f, 120.0f, 90.0f}, kUnitMetrics, scissor));
        CHECK(scissor.x == 0);
        CHECK(scissor.y == 0);
        CHECK(scissor.width == 100);
        CHECK(scissor.height == 80);
    }

    SECTION("degenerate and inverted rectangles are rejected")
    {
        CHECK_FALSE(ComputeOverlayScissor({10.0f, 10.0f, 10.0f, 20.0f}, kUnitMetrics, scissor));
        CHECK_FALSE(ComputeOverlayScissor({40.0f, 30.0f, 20.0f, 10.0f}, kUnitMetrics, scissor));
    }

    SECTION("DisplayPos is removed before conversion")
    {
        const OverlayDisplayMetrics metrics{100.0f, 50.0f, 1.0f, 1.0f, 100, 80};
        REQUIRE(ComputeOverlayScissor({110.0f, 62.0f, 170.0f, 110.0f}, metrics, scissor));
        CHECK(scissor.x == 10);
        CHECK(scissor.y == 12);
        CHECK(scissor.width == 60);
        CHECK(scissor.height == 48);
    }

    SECTION("Retina framebuffer scale doubles coordinates and extents")
    {
        const OverlayDisplayMetrics metrics{0.0f, 0.0f, 2.0f, 2.0f, 200, 160};
        REQUIRE(ComputeOverlayScissor({10.0f, 12.0f, 70.0f, 60.0f}, metrics, scissor));
        CHECK(scissor.x == 20);
        CHECK(scissor.y == 24);
        CHECK(scissor.width == 120);
        CHECK(scissor.height == 96);
    }

    SECTION("fractional origins and extents match the ImGui Metal backend")
    {
        REQUIRE(ComputeOverlayScissor({10.7f, 12.4f, 20.2f, 30.9f}, kUnitMetrics, scissor));
        CHECK(scissor.x == 10);
        CHECK(scissor.y == 12);
        CHECK(scissor.width == 9);
        CHECK(scissor.height == 18);
    }

    SECTION("sub-pixel extents collapse after reference-backend truncation")
    {
        CHECK_FALSE(ComputeOverlayScissor({0.8f, 12.0f, 1.2f, 20.0f}, kUnitMetrics, scissor));
        CHECK_FALSE(ComputeOverlayScissor({10.0f, 4.2f, 20.0f, 4.9f}, kUnitMetrics, scissor));
    }

    SECTION("DisplayPos and Retina scale are applied together")
    {
        const OverlayDisplayMetrics metrics{100.0f, 50.0f, 2.0f, 2.0f, 200, 160};
        REQUIRE(ComputeOverlayScissor({110.25f, 62.5f, 170.75f, 110.25f}, metrics, scissor));
        CHECK(scissor.x == 20);
        CHECK(scissor.y == 25);
        CHECK(scissor.width == 121);
        CHECK(scissor.height == 95);
    }
}
