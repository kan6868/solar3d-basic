-- animals.lua: a herd of animals (Kenney Animal Kit, glTF: assets/anim/animal-*.glb) walking around the floor.
--
-- Every .glb has its parts as nodes (body, legs, tail, wings...) and 8 node animations: static, idle, walk, run, eat,
-- dance, gesture-positive, gesture-negative. s3d.loadGLTF plays them (one draw call per animal); the shared texture
-- Textures/colormap.png is read next to the .glb files.
--
--   local animals = require( "animals" )
--   local herd = animals.newHerd( s3d, scene, { center = { 0, 0, -1 } } )
--   every frame: herd:update( dt ); herd:setPlaying( false ) stops the whole herd
local M = {}

local DIR = "assets/anim/"

local sin, cos, pi = math.sin, math.cos, math.pi

-- motion styles: clip to play, meters walked per cycle at scale 0.6, height above the floor (flying animals)
local STYLES = {
    walk   = { clip = "walk", stride = 0.45 },
    hop    = { clip = "walk", stride = 0.6 },
    waddle = { clip = "walk", stride = 0.3 },
    fly    = { clip = "walk", stride = 0.9, altitude = 0.9 },
    crawl  = { clip = "walk", stride = 0.25 },
}

-- the animals: style, scale, speed (m/s), heading offset (degrees; the crab walks sideways)
M.CATALOG = {
    { "beaver", "walk", 0.55, 0.7 },
    { "bee", "fly", 0.45, 1.1 },
    { "bunny", "hop", 0.5, 1.1 },
    { "cat", "walk", 0.5, 1.0 },
    { "caterpillar", "crawl", 0.5, 0.35 },
    { "chick", "waddle", 0.45, 0.6 },
    { "cow", "walk", 0.75, 0.8 },
    { "crab", "walk", 0.45, 0.6, 90 },
    { "deer", "walk", 0.7, 1.2 },
    { "dog", "walk", 0.55, 1.2 },
    { "elephant", "walk", 0.95, 0.7 },
    { "fox", "walk", 0.55, 1.2 },
    { "giraffe", "walk", 1.0, 0.9 },
    { "hog", "walk", 0.6, 0.9 },
    { "koala", "walk", 0.5, 0.6 },
    { "lion", "walk", 0.75, 1.0 },
    { "monkey", "walk", 0.55, 1.0 },
    { "panda", "walk", 0.65, 0.7 },
    { "parrot", "waddle", 0.5, 0.6 },
    { "penguin", "waddle", 0.5, 0.55 },
    { "pig", "walk", 0.6, 0.8 },
    { "polar", "walk", 0.7, 0.8 },
    { "tiger", "walk", 0.75, 1.1 },
}

-- ---------------------------------------------------------------------------------------------
-- Create one animated animal (not positioned yet). The returned model has moveTo / rotateTo / pause / resume / play.
function M.newAnimal( s3d, parent, name, style, opts )
    opts = opts or {}
    local S = STYLES[style] or STYLES.walk
    local speed = opts.speed or 1
    local scale = opts.scale or 0.6
    local obj = s3d.loadGLTF( parent, DIR .. "animal-" .. name .. ".glb", {
        scale = scale, castShadow = opts.castShadow ~= false,
    } )
    -- step rate matches the walking speed: (m/s) / (m per cycle) = cycles per second; the clip lasts one cycle
    local cyclesPerSec = speed / ( S.stride * scale / 0.6 )
    local clip = S.clip
    if not obj:getDuration( clip ) or obj:getDuration( clip ) <= 0 then clip = 1 end
    obj:play( clip, { fade = 0, speed = cyclesPerSec * obj:getDuration( clip ) } )
    obj.altitude = S.altitude or 0
    return obj
end

-- ---------------------------------------------------------------------------------------------
-- A herd walking on circles around center. opts:
--   center = { x, 0, z }, rings = { { radius = 5.2, dir = 1 }, { radius = 7, dir = -1 } },
--   names = { "cat", "dog", ... } (default: the whole kit; 10 animals on phones), castShadow = true,
--   perFrame = 1 (animals created per frame: loading gradually avoids a freeze at startup)
function M.newHerd( s3d, parent, opts )
    opts = opts or {}
    local center = opts.center or { 0, 0, 0 }
    local rings = opts.rings or { { radius = 5.2, dir = 1 }, { radius = 7, dir = -1 } }
    local list = {}
    local want = {}
    if opts.names then for _, n in ipairs( opts.names ) do want[n] = true end end
    for _, e in ipairs( M.CATALOG ) do
        if not opts.names or want[e[1]] then list[#list + 1] = e end
    end
    local maxCount = opts.max or ( s3d.isMobile and 5 or #list )
    while #list > maxCount do table.remove( list, ( #list % 2 == 0 ) and #list or 1 ) end

    local herd = { members = {}, playing = true }
    -- spread the animals evenly over each ring
    local perRing = {}
    for i = 1, #list do
        local r = ( ( i - 1 ) % #rings ) + 1
        perRing[r] = ( perRing[r] or 0 ) + 1
    end
    local slot = {}
    local queue = {}
    for i, e in ipairs( list ) do
        local r = ( ( i - 1 ) % #rings ) + 1
        slot[r] = ( slot[r] or 0 ) + 1
        queue[#queue + 1] = { entry = e, ring = rings[r], angle = 2 * pi * ( slot[r] - 1 ) / perRing[r] + r * 0.4 }
    end

    local function place( m )
        local R = m.ring.radius
        local x, z = center[1] + R * cos( m.angle ), center[3] + R * sin( m.angle )
        m.obj:moveTo( x, m.obj.altitude, z )
        -- animals face local +Z; turn along the tangent (rotate 180 degrees on clockwise rings)
        local heading = -math.deg( m.angle ) + ( m.ring.dir < 0 and 180 or 0 ) + ( m.yawOffset or 0 )
        m.obj:rotateTo( 0, heading, 0 )
    end

    local function spawn( q )
        local e = q.entry
        local ok, obj = pcall( M.newAnimal, s3d, parent, e[1], e[2], { scale = e[3], speed = e[4], castShadow = opts.castShadow } )
        if not ok then print( "animals: " .. tostring( obj ) ); return end
        local m = { obj = obj, name = e[1], ring = q.ring, angle = q.angle, speed = e[4], yawOffset = e[5] }
        if not herd.playing then obj:pause() end
        herd.members[#herd.members + 1] = m
        place( m )
    end

    local nextIndex = 1
    local perFrame = opts.perFrame or 1
    local function onFrame()
        for _ = 1, perFrame do
            local q = queue[nextIndex]
            if not q then Runtime:removeEventListener( "enterFrame", onFrame ); return end
            nextIndex = nextIndex + 1
            spawn( q )
        end
    end
    Runtime:addEventListener( "enterFrame", onFrame )

    function herd:update( dt )
        if not self.playing then return end
        for _, m in ipairs( self.members ) do
            m.angle = m.angle + m.ring.dir * m.speed / m.ring.radius * dt
            place( m )
        end
    end
    function herd:setPlaying( on )
        self.playing = on and true or false
        for _, m in ipairs( self.members ) do if on then m.obj:resume() else m.obj:pause() end end
    end
    function herd:count() return #self.members, #queue end
    return herd
end

return M
