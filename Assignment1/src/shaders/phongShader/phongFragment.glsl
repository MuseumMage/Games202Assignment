#ifdef GL_ES
precision mediump float;
#endif

// Phong related variables
uniform sampler2D uSampler;
uniform vec3 uKd;
uniform vec3 uKs;
uniform vec3 uLightPos;
uniform vec3 uCameraPos;
uniform vec3 uLightIntensity;

varying highp vec2 vTextureCoord;
varying highp vec3 vFragPos;
varying highp vec3 vNormal;

// Shadow map related variables
#define NUM_SAMPLES 20
#define BLOCKER_SEARCH_NUM_SAMPLES NUM_SAMPLES
#define PCF_NUM_SAMPLES NUM_SAMPLES
#define NUM_RINGS 10

#define EPS 1e-3
#define PI 3.141592653589793
#define PI2 6.283185307179586

uniform sampler2D uShadowMap;

varying vec4 vPositionFromLight;

highp float rand_1to1(highp float x ) { 
  // -1 -1
  return fract(sin(x)*10000.0);
}

highp float rand_2to1(vec2 uv ) { 
  // 0 - 1
	const highp float a = 12.9898, b = 78.233, c = 43758.5453;
	highp float dt = dot( uv.xy, vec2( a,b ) ), sn = mod( dt, PI );
	return fract(sin(sn) * c);
}

float unpack(vec4 rgbaDepth) {
    const vec4 bitShift = vec4(1.0, 1.0/256.0, 1.0/(256.0*256.0), 1.0/(256.0*256.0*256.0));
    return dot(rgbaDepth, bitShift);
}

float getBias(){
  vec3 normal = normalize(vNormal);
  vec3 lightDir = normalize(uLightPos);
  // return max(0.05 * (1.0 - dot(normal, lightDir)), 0.005);
  return 0.005;
}

vec2 poissonDisk[NUM_SAMPLES];

void poissonDiskSamples( const in vec2 randomSeed ) {

  float ANGLE_STEP = PI2 * float( NUM_RINGS ) / float( NUM_SAMPLES );
  float INV_NUM_SAMPLES = 1.0 / float( NUM_SAMPLES );

  float angle = rand_2to1( randomSeed ) * PI2;
  float radius = INV_NUM_SAMPLES;
  float radiusStep = radius;

  for( int i = 0; i < NUM_SAMPLES; i ++ ) {
    poissonDisk[i] = vec2( cos( angle ), sin( angle ) ) * pow( radius, 0.75 );
    radius += radiusStep;
    angle += ANGLE_STEP;
  }
}

void uniformDiskSamples( const in vec2 randomSeed ) {

  float randNum = rand_2to1(randomSeed);
  float sampleX = rand_1to1( randNum ) ;
  float sampleY = rand_1to1( sampleX ) ;

  float angle = sampleX * PI2;
  float radius = sqrt(sampleY);

  for( int i = 0; i < NUM_SAMPLES; i ++ ) {
    poissonDisk[i] = vec2( radius * cos(angle) , radius * sin(angle)  );

    sampleX = rand_1to1( sampleY ) ;
    sampleY = rand_1to1( sampleX ) ;

    angle = sampleX * PI2;
    radius = sqrt(sampleY);
  }
}

float findBlocker( sampler2D shadowMap,  vec2 uv, float zReceiver ) {
  poissonDiskSamples(uv);
  float numBlockers = 0.0;
  float avgBlockerDepth = 0.0;
  // float texelSize = 1.0 / 1024.0;
  float lightSize = 5.0 / 400.0;
  float nearPlane = 0.01;

  float searchRadius = lightSize * (vPositionFromLight.z - nearPlane) / vPositionFromLight.z;
  for (int i = 0; i < BLOCKER_SEARCH_NUM_SAMPLES; i++) {
    vec2 offset = poissonDisk[i] * searchRadius;
    float shadowDepth = unpack(texture2D(shadowMap, uv + offset));
    if (zReceiver - getBias() > shadowDepth + EPS) {
      avgBlockerDepth += shadowDepth;
      numBlockers ++;
    }
  }

  if (numBlockers == 0.0) {
    return -1.0;
  }
  return avgBlockerDepth / float(numBlockers);

  // return 1.0;
}

float useShadowMap(sampler2D shadowMap, vec4 shadowCoord){
  float closestDepth  = unpack(texture2D(shadowMap, shadowCoord.xy));
  float currentDepth = shadowCoord.z;
  float visibility = currentDepth - getBias() > closestDepth + EPS ? 0.0 : 1.0;
  // float visibility = currentDepth > closestDepth + EPS ? 0.0 : 1.0;
  return visibility;
}

float PCF(sampler2D shadowMap, vec4 coords, float sampleRadius) {
  // uniformDiskSamples(coords.xy);
  poissonDiskSamples(coords.xy);
  float visibility = 0.0;
  float currentDepth = coords.z;
  // float texelSize = 1.0 / 1024.0;
  for (int i = 0; i < PCF_NUM_SAMPLES; i++) 
  {
    vec2 offset = poissonDisk[i] * sampleRadius;
    float shadowDepth = useShadowMap(shadowMap, coords + vec4(offset, 0.0, 0.0));
    visibility += currentDepth - getBias() > shadowDepth + EPS ? 0.0 : 1.0;
  }
  return visibility / float(PCF_NUM_SAMPLES);
  // return 1.0;
}

float PCSS(sampler2D shadowMap, vec4 coords){

  // STEP 1: avgblocker depth
  float zReceiver = coords.z;
  float avgBlockerDepth = findBlocker(shadowMap, coords.xy, zReceiver);
  if(avgBlockerDepth < -EPS) {
    return 1.0;
  }

  // STEP 2: penumbra size
  float lightSize = 5.0 / 400.0;
  float penumbra = (zReceiver - avgBlockerDepth) * lightSize / avgBlockerDepth;

  // STEP 3: filtering
  return PCF(shadowMap, coords, penumbra);
  // return 1.0;
}

vec3 blinnPhong() {
  vec3 color = texture2D(uSampler, vTextureCoord).rgb;
  color = pow(color, vec3(2.2));

  vec3 ambient = 0.05 * color;

  vec3 lightDir = normalize(uLightPos);
  vec3 normal = normalize(vNormal);
  float diff = max(dot(lightDir, normal), 0.0);
  vec3 light_atten_coff =
      uLightIntensity / pow(length(uLightPos - vFragPos), 2.0);
  vec3 diffuse = diff * light_atten_coff * color;

  vec3 viewDir = normalize(uCameraPos - vFragPos);
  vec3 halfDir = normalize((lightDir + viewDir));
  float spec = pow(max(dot(halfDir, normal), 0.0), 32.0);
  vec3 specular = uKs * light_atten_coff * spec;

  vec3 radiance = (ambient + diffuse + specular);
  vec3 phongColor = pow(radiance, vec3(1.0 / 2.2));
  return phongColor;
}

void main(void) {

  float visibility;
  // 参考https://learnopengl-cn.github.io/05%20Advanced%20Lighting/03%20Shadows/01%20Shadow%20Mapping/ 的ShadowCalculation函数
  vec3 shadowCoord = vPositionFromLight.xyz / vPositionFromLight.w;
  shadowCoord = shadowCoord * 0.5 + 0.5;

  float PCFRadius = 5.0;
  float sampleRadius = 1.0 / 1024.0 * PCFRadius;

  // 代码参考 https://github.com/DrFlower/GAMES_101_202_Homework/blob/main/Homework_202/Assignment1/homework1/src/shaders/phongShader/phongFragment.glsl
  // visibility = useShadowMap(uShadowMap, vec4(shadowCoord, 1.0));
  // visibility = PCF(uShadowMap, vec4(shadowCoord, 1.0), sampleRadius);
  visibility = PCSS(uShadowMap, vec4(shadowCoord, 1.0));

  vec3 phongColor = blinnPhong();

  gl_FragColor = vec4(phongColor * visibility, 1.0);
  // gl_FragColor = vec4(phongColor, 1.0);
}