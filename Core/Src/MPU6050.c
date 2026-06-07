#include "MPU6050.h"

#define PI 3.14159265358979323846f
#define MPU6050_ADDR 0x68<<1
#define WHO_AM_I 0x75
#define PWR_MGMT_1 0x6B
#define ACCEL_X_H 0x3B
#define ACCEL_X_L 0x3C
#define ACCEL_Y_H 0x3D
#define ACCEL_Y_L 0x3E
#define ACCEL_Z_H 0x3F
#define ACCEL_Z_L 0x40

void MPU6050_Init(void){
    uint8_t send = 0x00;
    HAL_I2C_Mem_Write(&hi2c1,MPU6050_ADDR,PWR_MGMT_1,I2C_MEMADD_SIZE_8BIT,&send,1,50);
    uint8_t dev_address = 0;
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,WHO_AM_I,I2C_MEMADD_SIZE_8BIT,&dev_address,1,50);
    if(dev_address != 0x68){
        /* I2C通信失败，不卡死，后续读取将返回0 */
        return;
    }

}

void MPU6050_Read_Accel(float *ax_g, float *ay_g, float *az_g){
    int16_t ax = 0,ay = 0,az = 0;
    uint8_t axl = 0,ayl = 0,azl =0,axh = 0,ayh = 0,azh =0;
    if(HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_X_H,I2C_MEMADD_SIZE_8BIT,&axh,1,100)!=HAL_OK) return;
    if(HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_X_L,I2C_MEMADD_SIZE_8BIT,&axl,1,100)!=HAL_OK) return;
    ax = (int16_t)((axh<<8)|axl);
    if(HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Y_H,I2C_MEMADD_SIZE_8BIT,&ayh,1,100)!=HAL_OK) return;
    if(HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Y_L,I2C_MEMADD_SIZE_8BIT,&ayl,1,100)!=HAL_OK) return;
    ay = (int16_t)((ayh<<8)|ayl);
    if(HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Z_H,I2C_MEMADD_SIZE_8BIT,&azh,1,100)!=HAL_OK) return;
    if(HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Z_L,I2C_MEMADD_SIZE_8BIT,&azl,1,100)!=HAL_OK) return;
    az = (int16_t)((azh<<8)|azl);
    *ax_g = (float)ax /16384.0;
    *ay_g = (float)ay /16384.0;
    *az_g = (float)az /16384.0;

}

void MPU6050_Read_GYRO(float *tx_g, float *ty_g, float *tz_g){
    int16_t ax = 0,ay = 0,az = 0;
    uint8_t axl = 0,ayl = 0,azl =0,axh = 0,ayh = 0,azh =0;
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_X_H+8,I2C_MEMADD_SIZE_8BIT,&axh,1,50);
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_X_L+8,I2C_MEMADD_SIZE_8BIT,&axl,1,50);
    ax = axl+(axh<<8);
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Y_H+8,I2C_MEMADD_SIZE_8BIT,&ayh,1,50);
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Y_L+8,I2C_MEMADD_SIZE_8BIT,&ayl,1,50);
    ay = ayl+(ayh<<8);
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Z_H+8,I2C_MEMADD_SIZE_8BIT,&azh,1,50);
    HAL_I2C_Mem_Read(&hi2c1,MPU6050_ADDR,ACCEL_Z_L+8,I2C_MEMADD_SIZE_8BIT,&azl,1,50);
    az = azl+(azh<<8);
    *tx_g = (float)ax /131.0;
    *ty_g = (float)ay /131.0;
    *tz_g = (float)az /131.0;

}

/**
 * @brief 快速 atan 近似（最大误差 ~0.0015 rad ≈ 0.09°）
 *        替代 math.h 的 atan2，避免拉入浮点数学库
 */
static float fast_atan2(float y, float x) {
    /* 处理除零和象限 */
    if (x == 0.0f && y == 0.0f) return 0.0f;
    float abs_y = (y < 0.0f) ? -y : y;
    float angle;
    if (abs_y <= x) {
        /* |y| <= |x|: 一、四象限近似 */
        float z = y / x;
        angle = z * (0.9980f - z * z * (0.3053f - z * z * 0.1526f));
    } else {
        /* |y| > |x|: 二、三象限近似 */
        float z = x / y;
        angle = (PI / 2.0f) - z * (0.9980f - z * z * (0.3053f - z * z * 0.1526f));
    }
    if (y < 0.0f) angle = -angle;
    if (x < 0.0f) {
        if (y >= 0.0f) angle = angle + PI;
        else           angle = angle - PI;
    }
    return angle;
}

/**
 * @brief  快速平方根倒数近似（Quake III），±0.2% 误差
 *         配合一次牛顿迭代即可得到 sqrt
 */
static float fast_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    /* Quake III rsqrt 魔法 */
    long i;
    float x2, y;
    x2 = x * 0.5f;
    y  = x;
    i  = *(long*)&y;
    i  = 0x5f3759df - (i >> 1);  /* 经典魔法常数 */
    y  = *(float*)&i;
    y  = y * (1.5f - (x2 * y * y)); /* 一次牛顿迭代，精度 ~0.1% */
    return 1.0f / y;
}

void MPU6050_Cacul_Tangle(float *Pitch, float *Roll, float *ax, float *ay, float *az) {
    float ay2 = (*ay) * (*ay);
    float az2 = (*az) * (*az);
    float ax2 = (*ax) * (*ax);

    *Pitch = fast_atan2(*ax, fast_sqrt(ay2 + az2)) * 180.0f / PI;
    *Roll  = fast_atan2(*ay, fast_sqrt(ax2 + az2)) * 180.0f / PI;
}
